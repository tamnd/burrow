# burrow

[![CI](https://github.com/tamnd/burrow/actions/workflows/ci.yml/badge.svg)](https://github.com/tamnd/burrow/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-BSD--3--Clause-blue.svg)](LICENSE)

The Go standard library, reimplemented in C. All 180 packages, all 23,730 exported declarations, no subset and no "the interesting 20%".

It ships the way SQLite ships. You download one `.c` and one `.h`, drop them in your tree, compile them with the compiler you already have, and pick which packages come along. No build system, no dependency manager, no Go toolchain, no code generator on your machine.

The name is for gophers. They dig burrows, and a burrow is a network of tunnels that all connect.

## Why

Go's standard library is the best designed general purpose library anyone has shipped, and almost nobody can use it. Every embedded target, every kernel adjacent daemon, every codebase that cannot take a Go runtime, and every language whose FFI speaks only C is locked out of `net/http`, `crypto/tls`, `encoding/json`, `time`, `regexp` and `os`. Not because those designs are Go specific, but because nobody did the work.

C programmers hand roll a worse version of `strings` in every project. They reach for a different HTTP library every time and each one has a different opinion about ownership. They get TLS from OpenSSL and spend a week on the API. Meanwhile there is a library sitting right there that solved all of it, with twenty years of design review and 440,000 lines of tests, and the only thing standing between the two is a port that nobody has attempted.

## What it looks like

<!-- not compiled: uses strings_to_upper and fmt_println, and the strings and fmt packages come in a later milestone -->
```c
#define BURROW_SHORT 1
#include "burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Str body = S("the quick brown fox");
    Slice words = strings_fields(a, body);

    for (int i = 0; i < words.len; i++)
        fmt_println(AT(Str, words, i));

    arena_free(&ar);
    return 0;
}
```

There is no prefix on anything you call. Most C libraries put one on every symbol because C has a single flat namespace, but burrow already has a namespace, which is Go's package name. `strings_contains` is separated from `bytes_contains` and from everything outside the library by a segment that was going to be there anyway, so a library prefix on top would be a namespace on a namespace repeated 23,730 times.

The C name is the Go name with the dot turned into an underscore and the case fixed.

| Go | burrow |
| --- | --- |
| `strings.Contains` | `strings_contains` |
| `net/http.ListenAndServe` | `http_listen_and_serve` |
| `encoding/json.Marshal` | `json_marshal` |
| `(*strings.Builder).WriteString` | `strings_builder_write_string` |
| `strings.Builder` | `StringsBuilder` |
| `io.Reader` | `IoReader` |
| `time.Time` | `Time` |
| `io.EOF` | `io_eof` |
| `time.Second` | `TIME_SECOND` |
| `http.StatusNotFound` | `HTTP_STATUS_NOT_FOUND` |

Types are CamelCase and functions are snake_case, which is GTK's convention. It gives Go's builtins a namespace they otherwise lack, keeps the dangerous lowercase words like `string` and `map` out of the global namespace, and lets you tell a type from a call without thinking about it.

Macros are the one exception and keep a `BURROW_` prefix, because a header that defines `DEFER` or `S` globally will break somebody's program and C has no scoping mechanism that prevents it. `#define BURROW_SHORT 1` turns on the bare spellings for programs that want them. CI compares every public name, including the ones for packages not written yet, with the symbols of about 1,200 libraries on Linux and macOS, from libc to libcurl to libselinux, and the few that clashed were renamed. If you ever hit a collision anyway, `#define BURROW_PREFIX bw` puts a prefix back on everything, and since you compile burrow from source that is one line rather than a repackaging exercise.

## Memory

Every function that can allocate takes an allocator as its first parameter. Uniformly, mechanically, with no exceptions.

<!-- not compiled: uses the strings package, which comes in a later milestone -->
```c
Str upper = strings_to_upper(a, name);
Slice parts = strings_split(a, path, S("/"));
```

This is Zig's convention and it is the only answer that works here. Go's API hands back heap objects everywhere because a garbage collector cleans up afterwards. Bolting a tracing GC onto a C library would force a runtime on every consumer and break every FFI host. A `_free` for every constructor means documenting ownership 23,730 times and getting it wrong some of those times. One rule that covers the entire surface beats both.

Arena is the default and it is what makes the ergonomics work. You make an arena, you pass it down, you free it once, and in between you write the code you would have written in Go. A Boehm GC backend is available for people who want Go's exact ergonomics, a tracking allocator finds leaks and use after free in CI, `heap` is `malloc` and `free` behind the same interface for people who want to manage it themselves, and `fixed` runs the whole library out of a buffer you hand it.

The full story is one page, and it is the one page worth reading before you write any burrow: [docs/guides/allocators.md](docs/guides/allocators.md).

## Numbers

Go's numeric types are C's numeric types. `int8` through `int64` are `int8_t` through `int64_t`, `int` is `Int` and is 64 bits on a 64 bit machine and 32 on a 32 bit one exactly as Go's is, and `float64` is `double`.

Keep writing `+`. What burrow adds is the handful of cases where C says the behaviour is undefined and Go says exactly what the answer is.

<!-- example: docs/examples/readme/tour.c#numbers -->
```c
Int n = int_add(a, b);          /* wraps, like Go. a + b is undefined if it overflows */
Int q = int_div(a, b);          /* b == 0 stops the program with Go's message */
Uint h = uint_shl(hash, 70);    /* zero, not h << 6, which is what x86 does */
Int i = int_from_float64(f);    /* saturates, NaN gives zero */
```

Signed overflow is the one to care about. A compiler that sees `a + 1 > a` is entitled to fold it to `true` and delete the overflow check you wrote, without a warning. Every one of these is a `static inline` that compiles to the same instruction the operator would have, so `int_add` is one `add` at `-O2` and `int_div` puts two compares in front of a divide that was going to cost thirty cycles anyway.

Details, including the one place where two correct Go implementations disagree and burrow has to pick a side: [docs/guides/numbers.md](docs/guides/numbers.md).

## Zero values and results

Go promises that the zero value of a type is a working value, and C gives you the same bit pattern for free. `Str s = {0}` is the empty string, a zeroed `Error` means nothing went wrong, a zeroed interface is nil. That holds for every type in the library, which is a constraint on the design rather than a happy accident: it is why `SyncMutex` is an atomic word rather than a `pthread_mutex_t`, and there is a test that takes the zero value of every public type and uses it.

Go returns two things where C returns one, so the extra results move to the end of the parameter list in Go's order, `error` goes last, and any of them can be `NULL` if you do not want it.

<!-- example: docs/examples/readme/tour.c#results -->
```c
Int n = strconv_atoi(s, &err);
Int m = strconv_atoi(s, NULL);   /* do not care why it failed */
```

Details: [docs/guides/conventions.md](docs/guides/conventions.md).

## Strings

A string is a pointer and a length, passed by value, and it is not NUL terminated.

<!-- example: docs/examples/readme/tour.c#strings -->
```c
Str name = S("burrow");
Str arg  = str_from_cstr(argv[1]);   /* borrows, does not copy */
printf(STR_FMT "\n", STR_ARG(name));
```

That is the same shape Go uses, and it is the only shape that works, because a Go string can contain a NUL byte and a `char *` cannot. `os.ReadFile` on a JPEG returns a perfectly legal Go string. Every substring is also free, since the result just points into the middle of the original, which is why `strings_split` can hand back a thousand pieces without a thousand allocations.

Details, including how to get a real C string back when you need one: [docs/guides/strings.md](docs/guides/strings.md).

## Runes

Those bytes are UTF-8 when you decide to read them that way, and the loop that does it is Go's `for i, r := range s`.

<!-- example: docs/examples/readme/tour.c#runes -->
```c
Int i;
Rune r;
for (StrIter it = str_runes(s); str_next_rune(&it, &i, &r);)
    printf("%lld: %lx\n", (long long)i, (unsigned long)r);
```

`i` is the byte offset, not a count of runes, because the next thing you want after finding a rune is usually the text on either side of it. Underneath is `unicode/utf8`, ported whole, so `utf8_decode_rune_in_string`, `utf8_encode_rune`, `utf8_rune_count_in_string` and `utf8_valid_string` are all there with Go's exact behaviour on input that is not valid UTF-8.

That behaviour is the part worth knowing. Nothing in the package fails. A bad sequence decodes as U+FFFD with a width of one byte, so a loop over a corrupt file prints mojibake and terminates instead of hanging, and it never skips a byte that might have started something valid. Overlong encodings and surrogate halves are rejected, which matters more than it sounds: a slash written the long way is not a slash here, and a filter that only looked for the short one is a filter somebody would have walked straight past.

For ASCII work, index the bytes and skip all of this. A newline is a newline at the byte level in UTF-8 and no multi byte sequence can contain one, which is why Go's own `strings.IndexByte` is a byte loop and so is ours. Details: [docs/guides/runes.md](docs/guides/runes.md).

## Numbers as text

`strconv` is Go's, all of it, with the same answers. Floats print as the shortest text that reads back to the same bits, parsing is correctly rounded, and quoting escapes exactly what Go escapes.

<!-- example: docs/examples/readme/tour.c#strconv -->
```c
Str pi = strconv_format_float(a, 3.141592653589793, 'g', -1, 64);
double back = strconv_parse_float(pi, 64, NULL);
Str quoted = strconv_quote(a, S("tab\there, \xff"));
```

A differential fuzzer runs the whole package against Go's own on the same input, and the benchmarks put parsing a little ahead of Go and formatting level with it. Details: [docs/guides/strconv.md](docs/guides/strconv.md).

## Printing

`fmt` prints the way Go does, with the same verbs, flags and output, because every operand carries its type descriptor with it.

<!-- example: docs/examples/readme/tour.c#fmt -->
```c
Int xs[] = {3, 1, 4};
fmt_printf_v("%-6s|%5.2f|%v|%q\n", "pi", 3.14159, slice_from(xs, 3, 3, TYPE_INT),
             'x');
```

`%v` prints anything, including structs and maps, a type with a `String` method prints through it, and a verb that does not fit its operand prints a note like `%!d(string=x)` instead of reading garbage off the stack. Details: [docs/guides/fmt.md](docs/guides/fmt.md).

## Types

Go's library leans on its type system far more than it looks like it does. `fmt` prints anything because it can ask the value what it is, `encoding/json` walks a struct nobody wrote code for, `sort` works on a slice of anything. None of that is possible in C unless the types describe themselves, so in burrow they do.

<!-- example: docs/examples/readme/tour.c#types -->
```c
const Type *t = TYPE_INT;
printf("%u bytes\n", t->size);

if (type_equal(TYPE_STRING, &a, &b))
    printf("same bytes\n");
```

A descriptor is a `const Type` in read only memory, one per type, shared by everything that mentions it. Pointing at one costs a word and initialising one costs nothing, because the linker did it. Kinds are numbered exactly the way `reflect.Kind` numbers them, since those numbers are observable through `fmt`.

Details: [docs/guides/types.md](docs/guides/types.md).

## Slices

A pointer, a length, a capacity, and the element's type descriptor. Go's header is three words and this one is four, and the fourth is what buys you one `append` that works for every element type without templates.

<!-- example: docs/examples/readme/tour.c#slices -->
```c
Slice xs = slice_make(a, TYPE_INT, 0, 16);
xs = APPEND(Int, a, xs, 42);
Int v = AT(Int, xs, 0);
```

Append matches Go exactly, including the part people rely on without being able to state it: when the capacity is already there the elements go into the existing backing array and every other slice over that array sees them. The growth progression is Go's `nextslicecap`, so a program tuned against Go's allocation count gets the same one here.

`AT` and `APPEND` are not wrappers. They expand to an inline fast path that knows the element size at the call site, which makes the copy a single store and keeps the slice header out of memory, and they fall through to the general version for anything the fast path cannot do. On x86-64 that is fourteen times faster than the call it replaced.

Details, including what it costs against Go and the one place the capacity numbers differ from Go's and why: [docs/guides/slices.md](docs/guides/slices.md).

## Interfaces

Two words, a vtable pointer and a data pointer, which is what Go's interface value is too. Implementing one is a static `const` vtable and a constructor, both written once next to your type, so satisfying an interface costs a few words of read only memory and a call through it is a load and an indirect call.

<!-- example: docs/examples/readme/tour.c#iface -->
```c
static Int counter_read(void *self, Slice p, Error *err);
static const IoReaderVT counter_reader_vt = {TYPE_OF(Counter), counter_read};

IoReader counter_as_io_reader(Counter *c) {
    IoReader r = {&counter_reader_vt, c};
    return r;
}

Int read_some(IoReader r, Slice buf, Error *err) {
    return CALL(r, read, buf, err);
}
```

A zeroed interface value is nil, so an interface field in a struct that came out of an allocator starts out nil without anybody writing a line to say so. Every vtable starts with a `const Type *self_type`, which is what makes `iface_assert`, Go's `v.(T)`, possible on a value that has already forgotten its concrete type.

Embedding is by member, so an `IoReadWriter`'s vtable holds an `IoReaderVT` and an `IoWriterVT` and narrowing to either one is the address of a member rather than a pointer cast that would be right for the first and wrong for the second.

Go's empty interface is `Any`, a type descriptor and a pointer, and it is what `fmt`'s arguments and `json_marshal`'s parameter become.

<!-- example: docs/examples/readme/tour.c#any -->
```c
Any v = ANY_VAL(TYPE_INT, Int, 42);
```

Details, including what `Any` costs you that Go's `any` does not, which is that the pointee has to outlive it: [docs/guides/interfaces.md](docs/guides/interfaces.md).

## Function values

A Go `func` is code plus the variables it captured, so a function value here is a pair of the same two words an interface is. Every function type in a Go signature gets a named type, and `BURROW_FUNC` declares one.

<!-- example: docs/examples/readme/tour.c#func -->
```c
BURROW_FUNC(Filter, bool, Str s);

static bool has_prefix(void *env, Str s);

Int count_go_lines(Slice lines) {
    PrefixEnv e = {S("go")};
    return count_if(lines, FN(Filter, has_prefix, &e));
}
```

The environment word is not optional. A library that takes a bare function pointer forces every caller who needs state to reach for a global, and then two callers cannot use it at once. `qsort` is that mistake and every platform has since grown a `qsort_r` to undo it. Nothing in burrow takes a bare function pointer.

The function pointer is first, so a zeroed value is nil the same way a zeroed interface is, and the environment goes in first at the call, so the target declares it and ignores it rather than anybody casting a function pointer to a different signature, which is undefined behaviour and a trap under wasm or control flow integrity.

Details, including the one thing that needs thinking about, which is where the environment lives when the value outlives the frame that made it: [docs/guides/functions.md](docs/guides/functions.md).

## io

`io.Reader` and `io.Writer`, the two interfaces everything that moves bytes speaks, plus `Closer`, `Seeker` and the combinations, the sentinel errors with Go's messages, and the four functions that need nothing but an interface to run.

<!-- example: docs/examples/readme/tour.c#io -->
```c
int64_t n = io_copy(a, dst, src, &err);
```

`io_read_full` and `io_read_at_least` are ported line for line from Go's, including the distinction that makes them worth having: an input that ends before anything arrives is `io_eof` and an input that ends halfway through is `io_err_unexpected_eof`, which is the difference between there was no next record and the file is truncated.

The implementations that fill these in live where they live in Go, which is `os` and `bytes`, and neither of those is written yet.

## Errors

Go's `error` is an interface with one method, so burrow's is an interface value: a vtable pointer and a data pointer, returned by value, and a zeroed one means nothing went wrong.

<!-- not compiled: uses the os package, which comes in a later milestone -->
```c
Error err = os_write_file(path, data, 0644);
if (FAILED(err))
    return err;

if (errors_is(err, os_err_not_exist)) { ... }
```

Succeeding costs nothing, since there is no allocation on the happy path and nothing to free. The several hundred sentinels in Go's library, `io.EOF` and friends, are static `const` objects in read only memory here, so comparing against one is two pointer loads and creating one is a line the linker resolves. `errors_is` and `errors_as` are ports of Go's, walk for walk, including the `Unwrap() []error` trees that `errors.Join` builds.

`errors_as` returns the pointer instead of taking one and returning a bool, because in C the pointer is the bool:

<!-- not compiled: uses the os package, which comes in a later milestone -->
```c
const OsPathError *pe = errors_as(err, TYPE_OS_PATH_ERROR);
```

Details, including how to write your own error type and what happens when the allocator says no: [docs/guides/errors.md](docs/guides/errors.md).

## Maps

A Swiss table, which is what Go's map has been since 1.24. Keys and values are copied in and compared by value, so a map keyed by string is keyed by the bytes.

<!-- example: docs/examples/readme/tour.c#maps -->
```c
Map *counts = map_make(a, TYPE_STRING, TYPE_INT, 0);
MAP_SET(Str, Int, counts, S("the"), 1);

Int *n = MAP_GET(Str, Int, counts, S("the"));
if (n != NULL)
    (*n)++;
```

Eight slots to a group and eight control bytes in front of them, so asking which of the eight might hold your key is a few instructions on one 64 bit word, no branches and no key comparisons. A lookup that misses usually touches one cache line and compares nothing.

Iteration order is randomised per iterator, and that is on purpose. A program that depends on map order is already broken, and breaking it on the first run beats breaking it the day somebody adds a key.

Go's awkward corners come along unchanged because they are observable: a nil map reads as empty and writing to one stops the program, `-0.0` and `0.0` are one key, and every NaN key is a different key that can never be found again.

<!-- example: docs/examples/readme/tour.c#map-iter -->
```c
const void *k;
void *v;
for (MapIter it = map_iter(m); map_next(&it, &k, &v);)
    total += *(Int *)v;
```

Details, including the one deviation from Go, which is that an insert that grows the table invalidates a live iterator and says so: [docs/guides/maps.md](docs/guides/maps.md).

## Goroutines

Go's scheduler, with Go's algorithms and Go's names. A goroutine is a function on a stack of its own, put on a thread by the runtime rather than by the kernel.

<!-- example: docs/examples/readme/goroutines.c#goroutines -->
```c
static void worker(void *env) {
    Job *j = env;
    printf("working on job %d\n", j->id);
}

static void run(void *env) {
    (void)env;
    go(BURROW_FN(Func, worker, &job));
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}
```

`runtime_main` is the one call here that Go does not make you write. Go's runtime starts before your `main` because the toolchain arranges it, and since there is no toolchain here, something has to say where the goroutine world begins and ends. It starts the threads, runs the function you give it as the first goroutine, and joins everything before it returns.

A runnable goroutine is in one of three places: the `runnext` slot of a P, which holds the one that was just woken and keeps a handoff on one core, the lock free ring of 256 behind it, or the global queue. A thread with nothing to do looks in its own ring, then the global queue, then steals half of somebody else's, and gives its P up before it parks so that a wakeup cannot be lost.

`sched_park` and `sched_ready` are what channels, mutexes and the netpoller are all built on. Park a goroutine with the lock released by a callback rather than by the caller, and ready it when the thing it waited for happens.

One deviation from Go, and it is in the header. A goroutine stack is a fixed size decided once and never moved, because growing a stack means finding every pointer into it, which Go's compiler can do and nothing can do for C. The default is a quarter of a megabyte of lazily mapped address space, so a goroutine that touches one page costs one page.

Details: [docs/guides/goroutines.md](docs/guides/goroutines.md).

## Channels

<!-- example: docs/examples/readme/chan.c#chan -->
```c
static void producer(void *env) {
    Chan *c = env;
    for (Int i = 0; i < 10; i++)
        chan_send(c, &i);
    chan_close(c);
}

Int sum_all(Alloc *a) {
    Chan *c = chan_make(a, TYPE_INT, 0);
    go(BURROW_FN(Func, producer, c));

    Int v, sum = 0;
    while (chan_recv(c, &v))
        sum += v;

    chan_free(c);
    return sum;
}
```

That loop is Go's `for v := range c`, and it ends when the producer closes the channel.

A capacity of zero is unbuffered, which is a rendezvous rather than a queue of one: the value goes straight from the sender's variable into the receiver's, and by the time `chan_send` returns, some other goroutine has it. A positive capacity is a ring buffer and a send only blocks once it is full.

Values go by pointer and are copied through the type descriptor, so a channel of `Str` copies a `Str` properly and sending a loop variable does what it looks like it does. Closing is a broadcast and it is one way. `chan_try_send` and `chan_try_recv` are the select with a default arm that most code actually writes.

The rules that panic are Go's, exactly: send on closed, close of closed, close of nil. Each one means two parts of the program disagree about who owns the channel, and no return value would make such a program correct. The channel's lock is released before the panic goes up, so a catch block can survive one and the channel still works afterwards.

One thing Go has no equivalent of: a host thread that is not running a goroutine can block on a channel, because burrow is a library inside somebody else's program and that program has its own threads. It costs the thread, which a goroutine parking does not.

`chan_select` is Go's `select`, under a longer name because POSIX has the short one. You describe the arms and it tells you which one ran.

<!-- example: docs/examples/readme/chan.c#select -->
```c
Int job;
Str msg;

SelectCase cases[] = {
    BURROW_RECV(work, &job),
    BURROW_RECV(control, &msg),
    BURROW_DEFAULT,
};

switch (chan_select(cases, 3)) {
case 0: do_the_job(job); break;
case 1: obey(msg); break;
case 2: nothing_ready(); break;
}
```

No default arm means it waits. A default arm means it never waits. When more than one arm is ready the winner is picked uniformly at random, so a busy channel cannot starve a quiet one, and an arm on a `NULL` channel never fires, which is how a loop stops listening to a channel that has closed. Sixteen arms fit in your own stack frame with no allocation, and the locks are taken in channel address order so two selects listing the same channels the other way round cannot deadlock.

Details: [docs/guides/channels.md](docs/guides/channels.md).

## Sleeping and timers

<!-- example: docs/examples/readme/chan.c#timers -->
```c
time_sleep(500 * TIME_MILLISECOND);

TimeTimer *t = time_after_func(a, 5 * TIME_SECOND, BURROW_FN(Func, give_up, conn));
talk_to(conn);
time_timer_stop(t);
time_timer_free(t);
```

`time_sleep` is `time.Sleep` and it costs a timer, not a thread. The thread the goroutine was on goes and finds other work, so a program with a hundred thousand goroutines waiting on a hundred thousand deadlines is asleep in the kernel using no processor at all.

The timers are Go's, one set per P with a four way heap inside it, so a server that sets a deadline per request has every core pushing onto a different heap rather than queueing on one lock. Stopping a timer marks it and leaves it where it is, and the P that owns the heap throws it out later, which means a deadline set and cleared on every read never touches a heap.

One difference from Go, and it is the usual one. A timer has a lifetime and there is no collector, so `time_after_func` is paired with `time_timer_free`, which takes the timer out of whatever heap it is in before it hands the memory back. Arena users can ignore it.

`Duration` is a plain `int64_t` of nanoseconds, the same as Go's, with `TIME_SECOND` and the rest of the units.

Details: [docs/guides/time.md](docs/guides/time.md).

## defer

<!-- not compiled: uses the os package, which comes in a later milestone -->
```c
BURROW_SCOPE {
    OsFile *f = os_open(a, path, &err);
    if (BURROW_FAILED(err))
        return err;
    BURROW_DEFER(os_file_close, f);

    err = read_header(f);
}
BURROW_SCOPE_END;
```

The close runs when control leaves the block, however it leaves: off the end, `return`, `break`, `continue`, a `goto` out, or a panic unwinding past it. Calls run last in first out and the argument is read at the line you wrote it on, both of which are Go's.

The block is required and a `BURROW_DEFER` outside one does not compile. That is the point of it. A bare `BURROW_DEFER` is possible on GCC and Clang, which have the `cleanup` attribute, and impossible on MSVC, which does not, and a macro that silently leaks on one of three supported platforms is worse than one that costs a line on all three. Inside the block a defer is an ordinary statement and goes wherever a statement goes.

The one difference from Go is the unit: a scope rather than a function. Put the scope around the whole function body and you have Go's rule back. Leave it inside a loop and you get the thing Go programmers actually wanted, which is the file closed on every turn rather than n of them held open until the function returns.

A scope is one struct in your frame and the calls live in it, eight of them with nothing allocated, which is the same number Go's compiler open-codes into a frame. A scope that goes past eight takes one allocation that doubles as it fills and is freed before the scope returns, which is the trade Go makes for the defers it cannot put in the frame. The chain of open scopes belongs to the goroutine rather than the thread, so a goroutine that parks mid scope and wakes up elsewhere keeps its defers, and `runtime_goexit` runs all of them on the way out.

Details: [docs/guides/defer.md](docs/guides/defer.md).

## panic and recover

<!-- example: docs/examples/readme/tour.c#panic -->
```c
BURROW_TRY {
    parse(input);
}
BURROW_CATCH(p) {
    log_bad_input(panic_text(p));
    err = errors_new(a, BURROW_S("bad input"));
}
BURROW_TRY_END;
```

A panic unwinds until something catches it, running the deferred calls of every scope in between, innermost first. Unrecovered, it prints the value and ends the process with status 2, which is what Go does. The value is an `Any`, which is Go's `any`, which is what `recover` hands you there.

The one deviation from Go in the whole feature is where the recovery is written. Go recovers inside a deferred function and burrow recovers in a catch block, because resuming in the frame that recovered means a `setjmp` in that frame, and Go's rule would need one in every function that has a `defer` in it. Ours needs one only where somebody actually catches something, so a `defer` stays two stores and a call for everybody else.

Everything else is Go's, including the corners people forget: a panic inside a deferred call chains onto the one already unwinding and the rest of that scope's calls still run, `panic` with nil gets you a value that says so, and a panic does not cross a goroutine.

The runtime's own checks panic too, with an `Error` whose concrete type is `RuntimeError`, which is Go's `runtime.Error`. An index past the end, a slice expression past the capacity, a divide by zero, a write to a nil map, a send on a closed channel. `runtime_error_from(p)` in a catch block says whether the panic came from the runtime or from somebody's own code, and the messages are Go's text byte for byte.

Underneath it is `setjmp` and `longjmp` on all three platforms, with the unwinding done by hand rather than by a personality routine, so there is no unwinder to link, no tables, and no allocation on the panic path. Code with no `BURROW_TRY` in it pays nothing at all.

Details, including what a panicked value's lifetime actually is, which is the one thing that bites: [docs/guides/panic.md](docs/guides/panic.md).

## Atomics

<!-- example: docs/examples/readme/tour.c#atomics -->
```c
static int64_t hits;
sync_atomic_add_int64(&hits, 1);

static SyncAtomicInt64 served;
sync_atomic_int64_add(&served, 1);
```

Go's `sync/atomic`, both halves of it. The plain functions work on an object you already have, so a counter stays an `int64_t` in your own struct, and the types are the same operations bound to a value that can only be reached through them. Five integer widths with add, and, or, load, store, swap and compare and swap, plus the pointer set, plus `SyncAtomicBool` and `SyncAtomicValue`.

`sync_atomic_` rather than `atomic_` because `<stdatomic.h>` reserves the short prefix for names the C committee has not written yet, and a library that takes a reserved name is a library that stops building on some future compiler for reasons its users cannot fix. It is the import path with the slash turned into an underscore, and it is the whole of the difference from Go's names.

Everything except the four `SyncAtomicValue` calls is inline, so an add is one `lock xadd` on x86 and one `ldaddal` on arm64 with nothing around it. Sequential consistency throughout, which is the only ordering Go's package has. The acquire and release forms exist a layer down in `burrow/atomic.h` for code that has a reason.

Details, including how `Value` publishes two words without a lock and what happens on a 32 bit machine: [docs/guides/atomics.md](docs/guides/atomics.md).

## Locks

<!-- example: docs/examples/readme/tour.c#locks -->
```c
static SyncMutex mu;
static int balance;

void deposit(int n) {
    sync_mutex_lock(&mu);
    balance += n;
    sync_mutex_unlock(&mu);
}
```

Go's `sync.Mutex`, `sync.RWMutex` and `sync.Locker`. The zero value is unlocked, so there is nothing to initialise and nothing to destroy, and a lock in a static or in a struct you calloc'd is ready the moment its memory is zero.

These park. A goroutine waiting here gives its thread back to the scheduler and costs nothing but its own stack, so ten thousand goroutines queued on one mutex are ten thousand parked goroutines and not ten thousand blocked threads. A thread that is not a goroutine may take these locks too and sleeps instead, which is the part Go does not need and a library inside somebody else's program does.

Go's starvation handoff is ported rather than simplified. The lock goes to whoever is running, which is fast, until a waiter has been queued for more than a millisecond, and then it goes to the front of the queue until the queue drains. That is what stops a tight loop from starving a queue forever without paying a scheduling round trip on every unlock.

The fast paths are inline, so an uncontended lock is one compare and swap and about eleven nanoseconds, level with Go on the same machine.

Details, including why read locks do not nest and when an `RWMutex` is actually worth having: [docs/guides/sync.md](docs/guides/sync.md).

## Waiting, and doing something once

<!-- example: docs/examples/readme/tour.c#wait-group -->
```c
static SyncWaitGroup wg;

for (int i = 0; i < n; i++)
    sync_wait_group_go(&wg, BURROW_FN(Func, work, &jobs[i]));

sync_wait_group_wait(&wg);
```

Go's `sync.WaitGroup`. `sync_wait_group_go` adds one and starts the goroutine together, so the counter and the goroutine cannot get out of step. The zero value is a group with nothing in it. Go's misuse checks come with it: a counter taken below zero and an `Add` on a group somebody is already waiting on both stop the program, because the alternative is a bug that does not show up on the machine it was written on.

<!-- example: docs/examples/readme/tour.c#once -->
```c
static SyncOnce started;

sync_once_do(&started, BURROW_FN(Func, start, NULL));
```

Go's `sync.Once`. What it promises is that when any call returns, f has finished, which is stronger than "f runs once" and is why it is a mutex underneath rather than a compare and swap. The fast path is one atomic load, inline in the header.

`sync.OnceFunc`, `sync.OnceValue` and `sync.OnceValues` are here too. Go returns closures from those and burrow cannot, so they are structs you declare with an initialiser macro, nothing is allocated and there is nothing to free. They remember a panic and raise it again on every later call, which is the difference from a bare `Once` and the reason to reach for one.

<!-- example: docs/examples/readme/cond.c#cond -->
```c
static SyncMutex mu;
static SyncCond ready;
static bool has_work;

void consume(void) {
    sync_mutex_lock(&mu);
    while (!has_work)
        sync_cond_wait(&ready);
    take_the_work();
    sync_mutex_unlock(&mu);
}
```

Go's `sync.Cond`, for waiting until something you share with another goroutine changes. Waiting drops your lock, sleeps, and takes the lock again before it returns. `sync_cond_signal` wakes one waiter and `sync_cond_broadcast` wakes all of them. The `while` matters: a `Cond` promises you will be woken, not that the thing you were waiting for is true when you are. Unlike the other types here, a zeroed `Cond` isn't ready to use, since it has to know which lock it belongs to, so start up code sets `ready = SYNC_COND(sync_mutex_locker(&mu))` before anybody waits.

<!-- example: docs/examples/readme/tour.c#sync-map -->
```c
static SyncMap cache;
cache = SYNC_MAP(heap_allocator(), TYPE_STRING, TYPE_INT);

SYNC_MAP_STORE(Str, Int, &cache, BURROW_S("hits"), 1);

Int n;
if (SYNC_MAP_LOAD(Str, &cache, BURROW_S("hits"), &n))
    use(n);
```

Go's `sync.Map`, on the same hash trie Go has used since 1.24. A read takes no lock and writes nothing, so readers on different cores never take each other's cache lines away, and a write locks the one node that owns the slot it is changing. All ten of Go's methods are there. Reach for it in the two cases Go names, a key written once and then read over and over, or many goroutines touching mostly different keys, and reach for a plain `Map` behind a `SyncMutex` for everything else.

<!-- example: docs/examples/readme/tour.c#pool -->
```c
static SyncPool bufs;
bufs = SYNC_POOL(heap_allocator(), BURROW_FN(SyncPoolNewFunc, make_buf, NULL),
                 BURROW_FN(SyncPoolFreeFunc, drop_buf, NULL));

Any v = sync_pool_get(&bufs);
Buf *b = any_assert(v, TYPE_OF(Buf));
fill(b);
sync_pool_put(&bufs, v);
```

Go's `sync.Pool`, for the short lived object that gets made and thrown away a million times a second. Every P has a slot and a queue of its own, so a Get and a Put that stay on one P touch nothing another core is looking at. Go throws a pool's contents away at a garbage collection and there is no collector here, so the system monitor does it on a timer with Go's two generation rule kept intact, and the free function you supply is what an object goes to when its time is up.

Details: [docs/guides/sync.md](docs/guides/sync.md).

## Cancellation and deadlines

<!-- example: docs/examples/readme/context.c#cancel -->
```c
ContextCancelFunc cancel;
Context ctx = context_with_cancel(a, context_background(), &cancel);

sync_wait_group_go(&wg, BURROW_FN(Func, work, &ctx));
queue_the_jobs();
BURROW_CALLF0(cancel);
sync_wait_group_wait(&wg);
context_release(ctx);
```

<!-- example: docs/examples/readme/context.c#work -->
```c
static void work(void *env) {
    Context ctx = *(Context *)env;

    SelectCase cases[2];
    cases[0] = BURROW_RECV(context_done(ctx), NULL);
    cases[1] = BURROW_RECV(jobs, &job);

    for (;;) {
        if (chan_select(cases, 2) == 0)
            return;
        do_the_job(&job);
    }
}
```

Go's `context` package. A context is what a server hands down through every layer so that when the client hangs up, the database query, the two outbound requests and the retry loop underneath it all stop instead of finishing work nobody is waiting for. It is two words, it answers Go's four questions, and `context_done` gives you a channel that is closed when the work should stop, so waiting for a cancel is an ordinary `select` and costs nothing while it waits.

`context_with_timeout` is the same thing with a clock attached, so `context_with_timeout(a, parent, 5 * TIME_SECOND, &cancel)` gives the work five seconds and then closes the done channel underneath it. `context_with_deadline` takes the instant instead of the duration, for when several things share one. Either way the cancel function is still what stops the timer, and the error afterwards says which of the two arrived first.

`context_err` answers one of two sentinels, which is enough to decide whether to stop and not enough to explain afterwards why. `context_with_cancel_cause` hands you a cancel function that takes a reason with it and `context_cause` reads that reason back at the bottom of the call stack, without the error changing and without anybody threading it through by hand. `context_without_cancel` goes the other way, for the audit log or the metric a handler starts and does not wait for: it keeps everything the request was carrying and drops everything the request does about stopping.

`context_after_func` runs a function on a goroutine of its own once a context is cancelled, for the cleanup nobody is sitting in a `select` waiting to do, and hands back a stop function that says whether it got in before the cancellation did.

`context_with_value` carries a request id or an authenticated user down the same tree, under a key of a type private to whoever put it there. Cancellation reaches a context built on a parent from somebody else's code too, which takes one goroutine watching two channels, the same as it does in Go.

What is different is the ownership. Go leaves a context to the collector and there is none here, so every constructor takes an allocator and what it hands back is given back with `context_release`, parents after children. Freeing cancels first, so forgetting the cancel function cannot corrupt anything, and an arena user can skip the whole subject.

Details, including how to write your own `Context` and what each piece costs: [docs/guides/context.md](docs/guides/context.md).

## Tests for concurrent code that do not sleep

<!-- example: docs/examples/readme/synctest.c#synctest -->
```c
static void body(void *env) {
    (void)env;

    Chan *c = chan_make(heap_allocator(), TYPE_INT, 0);
    go(BURROW_FN(Func, worker, c));

    synctest_wait();

    /* The worker is sitting on the send. Not probably, not usually. */
    Int v;
    chan_recv(c, &v);
    chan_free(c);
}

void test_worker_blocks(void) {
    synctest_run(BURROW_FN(Func, body, NULL));
}
```

Go's `testing/synctest`. A test for something concurrent usually either sleeps for long enough that the thing under test has probably finished, which makes the suite slow and flaky in proportion to how loaded the machine is, or grows a pile of channels and wait groups that exist only so the test can tell when to look, which means the test is no longer testing the program that ships.

A bubble is the third way. Every goroutine started inside one belongs to it, and the bubble knows at every moment whether any of them can still make progress on its own. `synctest_wait` returns when none of them can, which is not a guess and not a timeout but a fact the scheduler already had. `synctest_run` returns when the last goroutine that was ever in the bubble has exited, so a goroutine a test forgot about is a test that does not finish rather than a surprise two tests later.

The question it answers is whether a goroutine is durably blocked, meaning the only thing that can wake it is another goroutine in the same bubble. A receive on a channel made inside the bubble is durable, a socket read is not, and the difference is decided at the park by the code that knows what is being waited for. A bubble where everything is durably blocked and nobody is waiting has deadlocked, and that stops the program rather than hanging it.

A bubble also has a clock of its own. It starts at midnight UTC on 1 January 2000, it moves only when every goroutine in the bubble is durably blocked, and then it jumps straight to the next timer that is due, so a test that sleeps for an hour takes microseconds and a thirty second timeout is something a test can wait for. `burrow_nanotime`, `time_sleep`, `time_after_func` and a `Context` deadline are all on it inside a bubble, and none of them had to be told about bubbles to be.

Details, including what a bubbled channel is and the two things about the clock that catch people out: [docs/guides/synctest.md](docs/guides/synctest.md).

## Tests the way Go writes them

<!-- example: docs/examples/testing/first.c#tests -->
```c
static void TestAbs(TestingT *t) {
    Int got = abs_int(-1);
    if (got != 1)
        testing_t_errorf_v(t, "abs_int(-1) = %d; want 1", got);
}

static void TestAbsZero(TestingT *t) {
    if (abs_int(0) != 0)
        testing_t_error_v(t, "abs_int(0) is not 0");
}

#define TESTS(X) X(TestAbs) X(TestAbsZero)
TESTING_MAIN(TESTS)
```

Go's `testing`. A test is a function that takes a `TestingT *`, `TESTING_MAIN` turns the list of them into a `main`, and the binary takes the flags `go test` passes, so `-test.v`, `-test.run`, `-test.count`, `-test.timeout`, `-test.shuffle` and `-test.failfast` all work and print what Go prints. Subtests, cleanups, skips, helpers and parallel tests are there too, and a failing test reports the file and line it failed on.

Details, including how failures read and what is not there yet: [docs/guides/testing.md](docs/guides/testing.md).

## Getting it

Two files, the way SQLite ships. Every release has an amalgamation archive with `burrow.c` and `burrow.h` in it. Add the first to your build, include the second, and link `-pthread` on Linux and macOS or `-lws2_32` on Windows. There is nothing to install and no build system to adopt.

```sh
cc -std=c11 -O2 main.c burrow.c -pthread -o main
```

From a checkout, `make` and CMake both build a static library, and `make amalgamation` generates the two files from the tree. Details, including how the pair is generated and how to check it against the source: [docs/guides/building.md](docs/guides/building.md).

## Status

Early. The core that every package stands on works, and every example on this page that doesn't need a package from a later milestone compiles and runs in CI, but the packages themselves are still to come.

The design is finished and written down in [docs/design](docs/design), twenty documents covering the scope measurement, the C dialect, the core types, the memory model, the scheduler, the reflection layer, the naming rules, every package tier, the conformance strategy, the build and deployment story, the phasing, and the legal position on porting a BSD licensed library.

The implementation plan is tracked in milestone issues:

- [P0: substrate, the part everything else stands on](https://github.com/tamnd/burrow/issues/1)
- [P1: the pure packages, strings through regexp](https://github.com/tamnd/burrow/issues/3)
- [P2: os, io/fs, time, and the platform layer](https://github.com/tamnd/burrow/issues/4)
- [P3: crypto](https://github.com/tamnd/burrow/issues/5)
- [P4: net and net/http](https://github.com/tamnd/burrow/issues/6)
- [P5: go/\*, templates, database/sql, log/slog](https://github.com/tamnd/burrow/issues/7)
- [P6: hardening and 1.0](https://github.com/tamnd/burrow/issues/8)

## Completeness is measured, not claimed

Go ships a machine readable manifest of its own API at `$GOROOT/api/go1.*.txt`, one line per exported declaration across every version. Because the naming rules are mechanical and reversible, a C symbol maps back to a Go declaration, so `tools/coverage` turns burrow's exported symbols into Go declarations and diffs them against that manifest. Coverage is a number a script prints, not an adjective in a README.

Correctness is measured the same way. Go's tests come across with the code, 440,000 lines of them, and a differential fuzzer runs burrow and Go against the same input and compares the output byte for byte. A package is not done until its coverage gate passes, its translated tests pass, its examples produce Go's exact output, it survives a day of fuzzing without diverging, it is clean under five sanitizers, its benchmarks are within the published ratios, it is green on every supported platform, and its documentation exists. Eight conditions, all of them checkable by a machine, none of them a matter of opinion.

## Repositories

| Repo | What |
| --- | --- |
| [burrow](https://github.com/tamnd/burrow) | The library, the amalgamation generator, the headers |
| [burrow-conformance](https://github.com/tamnd/burrow-conformance) | Translated Go tests, the differential oracle, fuzzing corpora |
| [burrow-testdata](https://github.com/tamnd/burrow-testdata) | Go's testdata files and generated vectors |
| [burrow-examples](https://github.com/tamnd/burrow-examples) | Go programs transliterated to C |

## Licence

BSD-3-Clause, matching Go, because a port of Go's source is a derivative work of Go and matching the upstream licence is the clean answer. Go's `PATENTS` grant travels with the code and is reproduced here. See [NOTICE](NOTICE) for attribution and [docs/design/18-legal.md](docs/design/18-legal.md) for the full position.

This project is not affiliated with, endorsed by, or sponsored by Google or the Go project. Go is a trademark of Google LLC.

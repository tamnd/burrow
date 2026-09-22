# Context

Cancellation, deadlines and request scoped values. This is `burrow/context.h`, and it is Go's `context` package.

A context is the thing a server hands down through every layer so that when the client hangs up, the database query, the two outbound requests and the retry loop underneath it all stop instead of finishing work nobody is waiting for. Go's own library takes one as the first argument to almost everything that can block, and so does burrow's.

Nothing in it is complicated on its own. A context is two words, it answers four questions, and the whole package is one tree with a cancellation that runs down it. What makes it worth a guide is that the ownership rules are new: Go leaves the lifetime of a context to the collector and there is no collector here.

## The four questions

```c
bool context_deadline(Context c, int64_t *when);
Chan *context_done(Context c);
Error context_err(Context c);
Any context_value(Context c, Any key);
```

These are Go's `Deadline`, `Done`, `Err` and `Value`, in Go's order, and they are the entire interface.

`context_done` hands back a channel that is closed when the work should stop. That is the one to build on, because a closed channel is ready in a `select` forever and never has to be asked twice.

`context_done` answers `NULL` for a context that is never cancelled, which is what `context_background` is. A `NULL` channel blocks forever and is never ready, exactly as Go's nil channel does, so a `select` on `context_done` is correct without asking which of the two it got.

`context_err` answers no error while the done channel is open, and afterwards it is `context_canceled` or `context_deadline_exceeded` and never changes again. Check it after the done channel fires, not instead of waiting on it.

`context_value` walks up the tree until it finds the key or runs out of parents, so a lookup costs one comparison per context in between. It is not a map.

Every one of the four stops the program on the nil `Context`, with the message Go prints for a method call on a nil interface value, because that is a bug in the caller and the message is worth more than the fault.

## Checking, and waiting

```c
static void work(void *env) {
    Context ctx = *(Context *)env;
    bool ok;

    while (!chan_try_recv(context_done(ctx), NULL, &ok))
        step();
}
```

That is the polling shape, for a loop that has something to do between checks. `chan_try_recv` on an open channel answers false and touches nothing, so the check is a load and a branch.

```c
SelectCase cases[2];

cases[0] = BURROW_RECV(context_done(ctx), NULL);
cases[1] = BURROW_RECV(jobs, &job);

if (chan_select(cases, 2) == 0)
    return;
do_the_job(&job);
```

And that is the blocking shape, which is the one to reach for. The goroutine is parked on both channels and costs nothing while it waits, and a cancel wakes it in the same few hundred nanoseconds any other channel close would.

Never send on a done channel and never close one. It belongs to the context.

## Making one

```c
CancelFunc cancel;
Context ctx = context_with_cancel(a, context_background(), &cancel);
if (BURROW_CONTEXT_IS_NIL(ctx))
    return err_no_memory;

go(BURROW_FN(Func, work, &ctx));
...
BURROW_CALLF0(cancel);
context_free(ctx);
```

`context_with_cancel` is `context.WithCancel`. It returns a copy of the parent that is also cancelled when the function it writes to `*cancel` is called.

`context_background` is the root of every tree. It is never cancelled, has no deadline, carries no values, costs nothing to make and needs no allocator and no free.

`context_todo` is the same thing with a note attached. It behaves identically and exists so that a reader can tell "no cancellation wanted" from "nobody has threaded a context through here yet". The two have different vtables, so a linter can tell them apart, which is what Go's static analysis does with them.

A nil `Context` comes back when the allocator will not give out the node and its channel. `*cancel` is set to a function that does nothing before anything can fail, so deferring the cancel before checking the context is correct rather than a crash.

Calling the cancel function is not optional. Until it runs, the context is still attached to its parent and the parent still holds a pointer to it, so a long lived parent with a cancel nobody called accumulates children forever. Go says the same thing and ships a vet check for it. Calling it twice, or a hundred times, does nothing after the first.

## Values

```c
/* In your own .c file, and nowhere else. */
static const Type request_id_type = { ... KIND_INT ... };
static Int request_id_key;

#define REQUEST_ID BURROW_ANY(&request_id_type, &request_id_key)

Context ctx = context_with_value(a, parent, REQUEST_ID, BURROW_ANY(TYPE_INT, &id));
...
Any v = context_value(ctx, REQUEST_ID);
if (!BURROW_ANY_IS_NIL(v))
    log_with_id(*(Int *)v.data);
```

`context_with_value` is `context.WithValue`. Use it for values that belong to the request rather than to the call and that cross an API boundary: a request id, an authenticated user, a trace span. Not for passing arguments, which is what arguments are for.

The key type should be private to whoever puts the value in, so that no two packages can collide on one. In Go that is an unexported named type. Here it is a `Type` descriptor declared `static` in your own `.c` file, and the effect is the same, because a key comparison compares descriptors before it compares anything else and nobody outside that file can name yours.

The key has to be comparable, since looking a value up compares keys, and a slice, a map or a function key stops the program the way Go's does.

Nothing is copied. Both `Any` values are two words and the context keeps them as they are, so whatever they point at has to outlive the context. For the usual case of a static key and a value owned by the request that is already true, and `any_box` is how to make a value that would not be.

## Giving it back

```c
context_free(ctx);
```

Go has no such call. The rule here is burrow's usual one: whoever made it frees it, and the order is the ordinary C order.

Free a context before the ones derived from it. A child holds a pointer to its parent for the value walk, so a parent that goes first leaves the child reading freed memory. Cancelling does detach a child from a cancellable parent, but a value context is not cancellable and has nothing to detach from, so do not lean on it.

`context_free` cancels first. That makes it safe on a context nobody cancelled, and it means anything still parked on the done channel is woken rather than left waiting on memory that is about to go. It is not a substitute for calling the cancel function at the right moment, only a guarantee that forgetting to does not corrupt anything.

`context_background`, `context_todo` and the nil context are all fine to pass and do nothing, so a cleanup path does not have to ask which kind it has. A context this package did not make stops the program, because the alternative is freeing a pointer to something whose shape is unknown.

An arena user can skip all of it. `arena_free` takes the whole tree at once, the same as everywhere else in burrow, and [guides/allocators.md](allocators.md) has the rest.

## Writing your own

`Context` is an interface, the same shape an `Error` is, so a wrapper or a context with a different source of cancellation is a vtable and a struct.

```c
static const ContextVT my_vt = {NULL, my_deadline, my_done, my_err, my_value};

Context c = {&my_vt, &my_state};
```

All four slots are required. Go's `Context` is an interface with four methods and a type implementing three of them does not implement `Context`, so there is no optional slot to leave `NULL`. That is different from `ErrorVT`, where Go finds `Unwrap`, `Is` and `As` by type assertion and their absence is normal.

`self_type` is the descriptor for whatever is behind `data`, for a caller that wants its own type back out of a `Context`. `NULL` means it does not want to be extracted, which is what an unexported type gets you in Go.

Two rules for a `done` that are worth stating because the package relies on them. It has to answer the same channel every time, and it must not fail, since there is nobody to report a failure to.

A `value` that does not know a key has to forward to whatever it wraps. Value lookup is the one operation every context in a chain forwards, and this package finds the nearest cancellable ancestor by asking for a value under a key of its own, so a wrapper that stops the walk cuts cancellation off at itself.

Everything works with a foreign parent, including cancellation, and the cost is one goroutine. A cancellable context built on a parent this package does not recognise gets a goroutine watching the parent's done channel and its own, which is what Go does in the same case. A foreign parent that answers `NULL` to `done` is never cancelled and costs nothing at all.

## What it costs

A `context_with_cancel` is one node and one channel, and attaching it to its parent allocates nothing: the children are an intrusive doubly linked list, so joining one is four pointer writes under the parent's lock. Go pays a map insert per `WithCancel` and cannot avoid it, since it has nowhere to put the links.

A `context_with_value` is one node, four words wide, and never takes a lock at all.

`context_done` is a load. The channel is made when the context is and never replaced, so nothing synchronises to read it. Go makes it lazily, under a lock, because Go can allocate anywhere and this cannot: `context_done` has no allocator and no way to report a failure. The trade is one channel per `WithCancel` whether or not anybody waits on it, and in exchange the read is free and Go's `closedchan` special case does not exist here.

A cancel walks the subtree once, closing each done channel and setting each error under that node's own lock. Nothing above the cancelled node is touched.

`context_value` is a pointer chase per context between the lookup and the value, with one `Any` comparison each. Keep the chain short if it is on a hot path, which is Go's advice too.

## What Go has that this does not, yet

`context.WithDeadline` and `context.WithTimeout` are next, and they are the reason `context_deadline` is in the interface already.

`context_deadline` answers an `int64_t` on the monotonic clock rather than a `Time`, because burrow has no calendar `Time` yet. It becomes a `Time` when the calendar half of the `time` package lands, and until then `burrow_nanotime` is the reading to compare it against. That is the part every caller actually uses: a deadline gets compared against now and subtracted from now, and both of those want the clock that cannot go backwards.

The Go 1.20 and 1.21 additions are not here: `WithCancelCause`, `Cause`, `WithoutCancel`, `WithDeadlineCause`, `WithTimeoutCause` and `AfterFunc`.

`context_deadline_exceeded` satisfies `net.Error` with `Timeout()` true in Go, so that code written against the network package treats it as a timeout rather than a hard failure. There is no `net` package here yet and it is a plain sentinel until there is.

## See also

- [guides/channels.md](channels.md) for `chan_select`, `BURROW_RECV` and what a closed channel does
- [guides/goroutines.md](goroutines.md) for `runtime_main`, `go` and the scheduler underneath
- [guides/interfaces.md](interfaces.md) for `Any`, `any_equal` and the two word interface value
- [guides/errors.md](errors.md) for what a sentinel error is and how to compare against one
- [guides/allocators.md](allocators.md) for what to pass as `Alloc *` and when the free can be skipped

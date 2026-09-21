# Sync

Go's `sync`, in `burrow/sync.h`. The locks a program written against this library reaches for, as opposed to the one the runtime uses on itself.

```c
#include "burrow/sync.h"

static SyncMutex mu;
static int balance;

void deposit(int n) {
    sync_mutex_lock(&mu);
    balance += n;
    sync_mutex_unlock(&mu);
}
```

The zero value is an unlocked mutex. There is nothing to initialise and nothing to destroy, so a mutex in a static, in a struct you calloc'd, or in an arena is ready the moment its memory is zero. That is Go's rule and it is the reason there is no `sync_mutex_init` here to forget to call.

## What makes these different from burrow/lock.h

They park. A goroutine waiting on one of these gives its thread back to the scheduler and costs nothing but its own stack, so ten thousand goroutines queued on one mutex are ten thousand parked goroutines and not ten thousand blocked threads.

`burrow/lock.h` cannot do that. It is the lock the runtime holds while it is deciding which goroutine runs next, and parking a goroutine is one of the things it is built out of. It is internal, its names carry the `burrow__` prefix, and nothing outside the runtime should be reaching for it.

Threads that are not goroutines may use the locks here too. burrow is a library inside somebody else's program and that program's own threads are allowed to take a lock. Such a thread sleeps rather than parking, so it costs itself a thread for as long as it waits, which is the price of not being a goroutine. Go is never in that situation and so has only the first half of this.

## The rules

A mutex is not reentrant. A goroutine that locks one it already holds waits forever for itself, exactly as in Go, and the answer is to stop doing that rather than to count. Recursive mutexes exist in other libraries and they make it possible to write a function that has no idea whether its caller was already inside the critical section, which is not a thing worth being able to do.

A mutex is not owned. Any goroutine may unlock a mutex that another goroutine locked. That sounds like a bug waiting to happen and it is also what makes a mutex usable as a one shot signal between two goroutines, which is a thing Go programs do. It is Go's rule and it is kept.

Do not copy a mutex once it has been used. A copy carries the same waiter counts as the original and neither of them is right afterwards. Go says this too and has `go vet` to say it at compile time. C has no vet, so it is said in the header and it is said here.

Unlocking a mutex that is not locked stops the program. There is no way to make that survivable, because the state that would have to be repaired is the state that says who is allowed to be in the critical section.

## Fairness, and the millisecond

The interesting part of Go's mutex is what happens when a lock is hot, and this is a port of that rather than a simplification of it.

A lock that always goes to whoever is running is fast, because the goroutine that just woke up is usually not the goroutine holding the processor, and handing the lock to the one that is already running skips a scheduling round trip. It is also a lock that can starve a queue forever: a goroutine looping on lock and unlock will win every race against a waiter that has to be woken first.

A lock that always goes to the front of the queue is fair and pays that scheduling round trip on every single unlock, which on a hot lock is most of what the program does.

Go's answer is to be the first kind until somebody has been waiting more than a millisecond, and the second kind until the queue drains. A waiter that crosses the threshold sets the starving bit, and then unlocking hands the lock straight to the goroutine at the front of the queue rather than letting whoever happens to be running take it first. As soon as the queue empties, or a waiter gets the lock inside the threshold, the mutex goes back to the fast mode where a lock is one compare and swap. The millisecond is the one number in the whole thing that was tuned rather than derived, and it is Go's number.

All of that lives in one `int32_t`: the lock bit, a woken bit, a starving bit, and the waiter count in the bits above. One word is what makes an uncontended lock a single compare and swap on a line the caller already has in cache.

## TryLock

```c
if (sync_mutex_try_lock(&mu)) {
    report(balance);
    sync_mutex_unlock(&mu);
} else {
    report_busy();
}
```

Takes the lock if it is free and answers whether it did. Never waits.

Go's warning comes with it: a correct use of this is rare, and code that loops on it is code that wanted `Lock`. It is here because the cases it is right for are real, such as a status page that would rather say "busy" than block, or a cleanup pass that can skip anything currently in use.

It is not a cheaper `Lock`, and measurably it is a dearer one. `Lock` goes straight at a compare and swap against zero. `TryLock` has to read the state first to find out whether the mutex is starving, because taking the lock out from under a queue that is being served in order is the one thing it must not do. That read is the whole difference.

## RWMutex

```c
static SyncRWMutex mu;

Entry *lookup(Str k) {
    sync_rw_mutex_r_lock(&mu);
    Entry *e = table_get(k);
    sync_rw_mutex_r_unlock(&mu);
    return e;
}

void insert(Str k, Entry *e) {
    sync_rw_mutex_lock(&mu);
    table_put(k, e);
    sync_rw_mutex_unlock(&mu);
}
```

Either one writer or any number of readers. The zero value is unlocked and the same rules about copying apply.

Writers queue against each other on an ordinary `SyncMutex` inside the struct. A writer announces itself to readers by subtracting a large constant from the reader count, which makes it negative in one atomic operation and so both announces the writer and tells every arriving reader to wait. The writer then waits for the readers that were already inside, and the last one out is the one that wakes it. A reader arriving after a writer is waiting queues behind that writer, which is what stops a steady stream of readers from starving writers forever.

The name is `sync_rw_mutex_` and not `sync_rwmutex_`, because `RW` is in the acronym table in [design/08-naming-abi.md](../design/08-naming-abi.md) and every acronym in a type name becomes its own segment. `RLock` becomes `sync_rw_mutex_r_lock` for the same reason.

## Read locks do not nest

A goroutine holding a read lock that takes the same read lock a second time deadlocks if a writer arrived in between. The second read lock queues behind the writer, the writer is waiting for the first read lock, and nobody moves.

Go documents exactly this. It is not an implementation detail that a better implementation would fix, because the alternative is letting readers starve writers indefinitely, and that is a worse bug that shows up later and on a bigger machine.

If a function might be called with the read lock already held, that fact belongs in its name or its comment. There is no way to ask a lock whether you are already inside it.

## When an RWMutex is worth having

Less often than people expect.

A read lock is a read modify write on a shared counter, so two readers on two cores contend for that cache line exactly as hard as two writers would. What an RWMutex buys is that the critical sections themselves run at the same time. It wins when the critical section is long enough for that parallelism to pay for the extra word of state and the extra branch, and a plain `SyncMutex` wins when the critical section is a pointer load and a compare.

Measure rather than assume, which is Go's advice as well. The numbers in [burrow-bench](https://github.com/tamnd/burrow-bench) have the uncontended cost of both on the same machine.

## Locker

A `SyncLocker` is something with a lock and an unlock, which is what a function that wants to hold a lock without caring which one takes.

```c
void with_lock(SyncLocker l, Func body) {
    sync_locker_lock(l);
    BURROW_CALLF0(body);
    sync_locker_unlock(l);
}

with_lock(sync_mutex_locker(&mu), BURROW_FN(Func, work, NULL));
with_lock(sync_rw_mutex_r_locker(&table), BURROW_FN(Func, scan, NULL));
```

Go needs no conversion function, because a `*Mutex` satisfies `Locker` by having the methods and the compiler does the rest. C has no structural typing, so the conversion has to be written down, and `sync_mutex_locker`, `sync_rw_mutex_locker` and `sync_rw_mutex_r_locker` are where it is written. Each result borrows its argument and is valid for exactly as long as that argument is.

It is a vtable pointer and a data pointer, the same shape as every other interface in the library, and the rules are explained once in [guides/interfaces.md](interfaces.md). A zeroed `SyncLocker` is nil, and locking or unlocking a nil one stops the program the same way calling a method on a nil interface does in Go.

`sync_rw_mutex_r_locker` is Go's `RLocker`. Locking it takes a read lock and unlocking it gives that read lock back, which is how a `Cond` waits on the read side of an `RWMutex`. `Cond` is the main reason `Locker` exists at all, and it is the next thing to be written.

## WaitGroup

```c
static SyncWaitGroup wg;

static void work(void *env) {
    Job *j = env;
    process(j);
}

for (int i = 0; i < n; i++)
    sync_wait_group_go(&wg, BURROW_FN(Func, work, &jobs[i]));

sync_wait_group_wait(&wg);
```

A counter with a queue on it. `sync_wait_group_add` moves the counter, `sync_wait_group_done` takes one off, and `sync_wait_group_wait` returns when the counter reaches zero. The zero value is a group with nothing in it, so again there is nothing to initialise.

`sync_wait_group_go` is Go's `WaitGroup.Go` and it is the call to reach for, because it adds one and starts the goroutine together and there is no window between them for anything to go wrong. It answers false when the goroutine could not be started and takes the one back off the counter first, so a group that failed to start anything is a group with nothing in it rather than a group nobody can wait on.

The function handed to `sync_wait_group_go` must not panic. A panic nobody recovers inside a goroutine ends the program, and it ends it with the counter still up. That is deliberate and it is Go's behaviour: taking one off on the way out of a panic would let a `Wait` somewhere else return and race the shutdown, which can mean a process that exits zero while it is in the middle of reporting a crash.

Two misuses stop the program rather than hanging somewhere else later. Taking the counter below zero, which is a `Done` too many. And adding to a group somebody is already waiting on, which means the `Add` that was supposed to happen before the `Wait` did not. Both are Go's checks and both are worth keeping, because the alternative is a bug that does not show up on the machine it was written on.

The counter and the waiter count live in one `uint64_t`, the counter in the high half and the waiters in the low. That is what lets `Add` take the counter down and find out whether anybody is waiting in a single atomic, which is the whole trick: the two questions have to be answered together or a wakeup goes missing.

An `Add` and a `Done` with nobody waiting is about thirteen nanoseconds on a server core, which is level with Go. Four goroutines started through `Go` and waited for is about 880 nanoseconds against Go's 2183, and that row is mostly the scheduler rather than the group.

## Once

```c
static SyncOnce started;

void ensure_started(void) {
    sync_once_do(&started, BURROW_FN(Func, start, NULL));
}
```

Runs one function one time, however many callers ask and however many ask at once.

What it promises is stronger than "f is called once", and the difference is the reason this is not just a compare and swap. When any call returns, f has finished. A compare and swap gives the first guarantee and not the second: the caller that loses the race would return straight away, past a thing that is still being built. So the slow path is a mutex, the flag is set after f returns rather than before it runs, and the loser waits on the mutex for the winner.

The fast path is one atomic load and a branch, inline in the header, which is all a `Once` that has already run ever costs. It measures at about a nanosecond and Go's at about half of one, and both figures are a loop rather than a `Once`: the body is a single load and the loop around it is larger than the body.

A different function on a later call is not run. One `Once` means one action, and a second action wants a second `Once`. Calling `sync_once_do` from inside f, on the same `Once`, deadlocks.

If f panics, the `Once` counts as done and later calls return without running anything. That is Go's rule and the reasoning is that f had its turn. It is also the reason the three wrappers below exist.

## OnceFunc, OnceValue and OnceValues

```c
static SyncOnceFunc setup = SYNC_ONCE_FUNC(BURROW_FN(Func, load, NULL));

sync_once_func_call(&setup);
```

The difference from a bare `Once` is what happens to a panic. A `Once` lets the panic out once and then reports the action as done, so every later caller carries on as if the thing had been built. These remember the panic and raise it again on every later call, so nobody gets a half built thing quietly. That is Go's behaviour for all three.

Go returns a closure from each of them and lets the collector clean it up. C has no closures and no collector, so the state is a struct you declare and the function is a call on it. Nothing is allocated and there is nothing to free, and `sync_once_func_fn` hands back a `Func` for the cases that want to pass it on rather than call it. The cost of that shape is that the caller has to find somewhere to put the struct, which in practice is a static or a field of whatever the thing belongs to. The gain is that the call is direct: `sync_once_func_call` is 2.70 nanoseconds on a server core against Go's 5.34, and `sync_once_value_get` is 4.06 against 5.22.

`SyncOnceValue` produces one value and `SyncOnceValues` produces two, because Go's second result is usually an error and dropping it would make the thing useless for the case it exists for.

```c
static Config cfg;

static Any load(void *env) {
    (void)env;
    cfg = read_config();
    return BURROW_ANY(TYPE_CONFIG, &cfg);
}

static SyncOnceValue config = SYNC_ONCE_VALUE(BURROW_FN(AnyFunc, load, NULL));

Config *c = any_assert(sync_once_value_get(&config), TYPE_CONFIG);
```

Go's `OnceValue` is generic in the result type and this one is an `Any`, for the reason every other place in the library that holds one value of any type is an `Any`: C has no type parameters, the descriptor is what carries the type, and the assertion on the way out is what a compiler would otherwise have checked. An `Any` points rather than holds, so what f returns has to outlive the `SyncOnceValue`, which is usually free here since the thing being computed once is usually a static.

One thing worth knowing about the panic these keep. A caught panic value lives in the frame that caught it, so keeping it for a later call means copying it out, and each of these three structs has thirty two bytes to copy it into. That is the same bargain and the same number `burrow/panic.h` makes for a catch block. A panic value bigger than that keeps pointing where it pointed, which in practice means do not panic with a large value you built on the stack.

## What it costs

An uncontended lock and unlock is about eleven nanoseconds on a server core, which is what one compare and swap and one atomic add cost on hardware nobody else is touching. A write lock and unlock on an `RWMutex` is about twice that, because it goes through the inner mutex first and then touches the reader count. A read lock and unlock is the same as a plain mutex, since both are one atomic on one word.

`sync_mutex_lock`, `sync_mutex_try_lock`, `sync_mutex_unlock`, `sync_rw_mutex_r_lock` and `sync_rw_mutex_r_unlock` are `static inline` in the header, with the slow halves out of line behind the `burrow__` prefix. That is deliberate and it was measured. Go's compiler inlines these fast paths into the caller, and a C port that puts the same one instruction behind a call across a library boundary roughly doubles what the uncontended case costs. Moving them into the header took the mutex row from twenty percent behind Go to level with it and changed nothing else.

Under contention, four goroutines locking an empty critical section as fast as they can, burrow is a little ahead of Go on the same machine. That is the case starvation mode exists for and the algorithm is the same algorithm, so level is the expected answer and anything else would mean the port lost something. The numbers and the machine they came from are in [burrow-bench](https://github.com/tamnd/burrow-bench).

## Underneath

Waiters queue on the runtime's semaphore, which is Go's, from `src/runtime/sema.go`. A semaphore here is a `uint32_t` that the caller owns, and the waiters for it live in a table of 251 roots shared by every semaphore in the program, keyed on the address of that word. Each root holds a balanced tree of addresses with the waiters for one address chained off its node, so a program with a million mutexes has one table and not a million queues, and a mutex nobody is waiting on has no queue anywhere.

Before queueing, a goroutine spins a few times, but only on a machine with more than one processor and only when there is a processor free to be running the lock holder. Spinning for a lock held by a goroutine that cannot be running is pure waste. The processor count that decision uses is the count of processors this process is allowed to run on rather than the count the machine has, which is a different number inside a container with a cpuset or under `taskset`.

## What is not here yet

`Cond`, `Map` and `Pool`, in that order. All three are on the P0 milestone and all three are built on what is above.

`Cond` needs one thing that does not exist yet, which is Go's `notifyList` from `src/runtime/sema.go`. The semaphore underneath these locks wakes one waiter at a time by address and a `Cond` needs to wake all of them by ticket, so that is a port of its own rather than a call into what is there.

## See also

- [guides/atomics.md](atomics.md) for `sync/atomic`, which is what these are built out of
- [guides/goroutines.md](goroutines.md) for what parking actually does
- [guides/interfaces.md](interfaces.md) for the shape of `SyncLocker`
- [design/06-runtime.md](../design/06-runtime.md) for the semaphore and the scheduler

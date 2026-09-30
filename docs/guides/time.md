# Time

Durations, instants, the calendar and time zones, sleeping, and running a function later on. This is `burrow/time.h`: `Duration` and its units, `Time` with its dates and zones, `time.Sleep`, and `time.AfterFunc` with the `Stop` and `Reset` that go with it.

Formatting and parsing, `Format`, `Parse` and the layouts, are not here yet, and neither are the timers that hand back a channel. The end of this page says what is missing.

## Durations

<!-- example: ../examples/time/clock.c#durations -->
```c
Duration d = 500 * TIME_MILLISECOND;
Duration total = 2 * TIME_SECOND + 300 * TIME_MILLISECOND;
```

`Duration` is a count of nanoseconds and it is a plain `int64_t`, which is what Go's is. So `d / 2`, `d * 3` and `d1 < d2` all mean what they look like, and printing one takes the format for a long long.

Signed, because the difference between two readings of a clock can be negative, and 64 bits wide, which puts the longest duration at about 292 years. Anything longer than that is a date rather than a duration.

The units are `TIME_NANOSECOND`, `TIME_MICROSECOND`, `TIME_MILLISECOND`, `TIME_SECOND`, `TIME_MINUTE` and `TIME_HOUR`, and they are Go's constants with the names the mapping in [design/08-naming-abi.md](../design/08-naming-abi.md) gives them. Write the multiplication rather than the nanoseconds. `500000000` and `5000000000` look alike at a glance, one of them is ten times the other, and that is a bug that has shipped in real programs more than once.

## Reading the clock

<!-- example: ../examples/time/clock.c#clock -->
```c
int64_t start = burrow_nanotime();
work();
Duration took = burrow_nanotime() - start;
```

`burrow_nanotime` is nanoseconds on a clock that only goes forwards, measured from an arbitrary point that means nothing on its own. Subtract two readings and the answer is a `Duration`.

It says `burrow` rather than `time` because Go has no such function, which is what rule R11a in [design/08-naming-abi.md](../design/08-naming-abi.md) asks for. Go's `time.Now` carries a monotonic reading around inside it and `time.Since` pulls it back out, so a Go program never names the clock directly. `time_now` and `time_since` do the same here, and the section on elapsed time below shows them. This is the reading on its own, for code that wants an elapsed time and has no use for a date. `burrow/context.h` measures a deadline on this clock, and so does the runtime.

Never backwards and never a jump, which is the point of it. The wall clock does both whenever somebody sets the date or ntp corrects a drift, and a timeout measured on the wall clock either waits an hour or fires twice.

Callable from any thread, including one the runtime knows nothing about, and it costs a few nanoseconds everywhere, because every platform answers this out of the vdso or its equivalent rather than from a system call.

Inside a synctest bubble this reads the bubble's clock instead. See the section on fake time below.

## Dates

<!-- example: ../examples/time/calendar.c#date -->
```c
Time t = time_date(2009, TIME_NOVEMBER, 10, 23, 0, 0, 0, time_utc_loc);
TimeDateRet d = time_date_of(t);
TimeClockRet c = time_clock(t);
```

`Time` is Go's `time.Time`, a value you pass and return by copy. It is three words, the same three Go has: the nanosecond and some flags, the seconds, and a pointer to the location. A `Time` with every field zero is January 1 of year 1 in UTC, which `time_is_zero` recognises, so a struct with a `Time` in it that nobody set reads as "not set" the way it does in Go.

`time_date` builds one and `time_date_of` takes it apart. The names differ because Go has a function `Date` and a method `Date` that do opposite things, and C only gets one of them. The same goes for `time_from_unix`, which builds a `Time` from a unix second, and `time_unix`, which reads one back. Everything that returns more than one value in Go returns a small struct here, so `time_clock` gives the hour, minute and second together.

The pieces have accessors of their own too: `time_year`, `time_month`, `time_day`, `time_weekday`, `time_year_day`, `time_iso_week` and the rest. `TimeMonth` and `TimeWeekday` are plain integers with Go's constants, and `time_month_string` and `time_weekday_string` give the English names. The allocator there is only used for a value out of range, and `NULL` is fine when you know it is in range.

<!-- example: ../examples/time/calendar.c#normalise -->
```c
Time n = time_date(2024, TIME_OCTOBER, 32, 0, 0, 0, 0, time_utc_loc);
Time m = time_add_date(time_date(2024, TIME_JANUARY, 31, 0, 0, 0, 0, time_utc_loc),
                       0, 1, 0);
```

Every field may be out of its range and carries into the next, which is Go's rule. October 32 is November 1, and January 31 plus one month is February 31, which is March 2 in a leap year. That surprises people in every language that does it, and the fix is the same in all of them: add months to the first of the month.

## Zones

<!-- example: ../examples/time/calendar.c#zones -->
```c
Error err = BURROW_NO_ERROR;
TimeLocation *ny = time_load_location(BURROW_S("America/New_York"), &err);
if (ny == NULL)
    ny = time_fixed_zone(heap_allocator(), BURROW_S("EST"), -5 * 60 * 60);
Time there = time_in(t, ny);
```

`time_load_location` is `time.LoadLocation`. It looks under `$ZONEINFO` first, which may name a directory or a zip file laid out like Go's `lib/time/zoneinfo.zip`, and then in the system's zoneinfo directory, and it reads the same TZif files Go reads with the same parser, including the TZ string at the end that says what happens after the last transition in the file. Each name is read once and kept for the life of the process, so the pointer never needs freeing and a second load of the name costs a lock and a string compare.

`time_utc_loc` and `time_local_loc` are Go's `time.UTC` and `time.Local`. Local follows `$TZ` the way Go's does on Unix: unset means `/etc/localtime`, empty means UTC, and anything else is a name to load.

Windows has no zoneinfo directory, so there `time_load_location` fails for every name and Local is UTC. The program above falls back to a fixed zone for that reason. Go reads the registry for Local and carries a copy of the database for the rest, and both are coming.

`time_fixed_zone` is `time.FixedZone`, and it takes an allocator because a named zone needs somewhere to keep its name. Give it back with `time_location_free`, which does nothing for a location it does not own, so it is safe on anything. An unnamed zone at a whole number of hours comes from a table and costs nothing, and passing `NULL` for the allocator is fine then. `time_load_location_from_tz_data` makes a location from the bytes of a TZif file, and is freed the same way.

`time_in` moves a `Time` into a location without changing the instant, so `there` and `t` compare equal with `time_equal`. Use `time_equal` rather than `memcmp` or `==` on the fields, because two readings of one instant can differ in their location and in whether they carry a monotonic reading.

## Arithmetic

<!-- example: ../examples/time/calendar.c#arithmetic -->
```c
Time later = time_add(t, 90 * TIME_MINUTE);
Duration gap = time_sub(later, t);
Time hour = time_truncate(later, TIME_HOUR);
```

`time_add` and `time_sub` are Go's `Add` and `Sub`. A difference too big for a `Duration`, which tops out at about 292 years, comes back as the largest or smallest one rather than wrapping. `time_truncate` and `time_round` work from the zero time and not from the location, so truncating to a day gives midnight UTC and not local midnight, which is Go's rule and catches people out in Go too.

`Duration` has Go's methods as functions: `duration_hours`, `duration_minutes` and `duration_seconds` as a `double`, `duration_truncate`, `duration_round` and `duration_abs`.

## Elapsed time

<!-- example: ../examples/time/calendar.c#elapsed -->
```c
Time start = time_now();
time_sleep(2 * TIME_MILLISECOND);
Duration took = time_since(start);
```

`time_now` reads the wall clock and the monotonic clock together and keeps both, and `time_since`, `time_until` and `time_sub` between two such readings use the monotonic one. So an elapsed time is right even when somebody sets the system date in the middle of it. Anything that makes a new instant rather than moving along the clock, like `time_in`, `time_truncate` or `time_add_date`, drops the monotonic reading, exactly as Go does.

## Sleeping

<!-- example: ../examples/time/sleep.c#poll -->
```c
static void poll(void *env) {
    (void)env;
    while (!sync_atomic_bool_load(&stopping)) {
        check_the_thing();
        time_sleep(30 * TIME_SECOND);
    }
}
```

`stopping` is a `SyncAtomicBool` that whoever wants the polling to end sets.

`time_sleep` is `time.Sleep`. The goroutine stops for at least `d` and the thread it was running on goes and finds something else to do, which is the difference between this and every sleep C has. A thousand goroutines sleeping is a thousand stacks and no threads, and while they are all waiting the process is asleep in the kernel using no processor at all.

At least `d`, and never exactly. The goroutine becomes runnable when the time is up and then waits for a thread to pick it up, so a busy program hands it back late. That is true of every sleep in every language. What this promises is what Go promises: not early, and measured on the monotonic clock, so nothing anybody does to the system date can make it come back sooner or later.

Zero or less returns immediately without giving up the thread, which is Go's rule too. It is not a yield. If a yield is what you want, that is `runtime_gosched`.

Calling it off a scheduler thread, from your own `main` or from a test, sleeps the thread instead. There is no goroutine to park, so that is the only thing it can do, and it is a courtesy for setup code rather than something to build on.

## Running a function later

<!-- example: ../examples/time/timer.c#after -->
```c
static void give_up(void *env) {
    conn_close(env);
}

static Error serve(Alloc *a, Conn *conn) {
    TimeTimer *t = time_after_func(a, 5 * TIME_SECOND, BURROW_FN(Func, give_up, conn));
    if (t == NULL)
        return burrow_err_out_of_memory;

    conn_read_request(conn);

    time_timer_stop(t);
    time_timer_free(t);
    return BURROW_NO_ERROR;
}
```

`time_after_func` is `time.AfterFunc`. The function runs in a goroutine of its own once the duration is up, which is Go's rule and matters more than it sounds: the callback has a fresh stack, so it may block, take locks, sleep again or talk to the network without holding up the thread that noticed the timer was due. The price is that two callbacks due at the same instant have no order between them, exactly as in Go.

It answers `NULL` when the allocator or the timer heap would not give out memory, and then nothing has been armed and nothing will run. Go cannot fail here because Go throws instead, and a library does not get to make that choice for the program using it.

It has to be called from a goroutine, because the timer goes into the heap of the P the caller is on.

## Stopping and moving one

<!-- example: ../examples/time/timer.c#stop -->
```c
bool was_waiting = time_timer_stop(t);
```

`Stop`. True means the timer was still waiting and now is not. False means it had already fired, or had already been stopped.

False does not mean the callback has finished. It does not even mean the callback has started, because a stop that loses the race by a nanosecond still says false while the goroutine is still being put together. Go has exactly this and the answer is the same in both: a stop does not synchronise with the callback, so anything the callback touches needs a lock of its own.

<!-- example: ../examples/time/timer.c#reset -->
```c
bool pending;
if (!time_timer_reset(t, 5 * TIME_SECOND, &pending))
    return burrow_err_out_of_memory;
```

`Reset`, and it arms the timer for `d` from now whether or not it was running. `pending` may be `NULL`, and when it is not it is set to whether the timer was still waiting, which is what Go's `Reset` returns. That answer is out here rather than in the return value because arming a timer that had already fired can need the P's heap to grow, and a heap that will not grow is a failure this has to report. A false return means the timer is not armed and will not run.

Go's advice about `Reset` applies unchanged. Resetting a timer whose callback is already running does not unrun it, and a program that needs to know which of the two happened has to say so itself, with a flag under a lock the callback takes as well.

A stop costs the timer's own lock and nothing else. The timer is left where it is and marked, and the P that owns the heap throws it out next time it walks one, so a connection that sets and clears a read deadline on every read never touches a heap at all. That is the case this design is for.

## Giving the memory back

<!-- example: ../examples/time/timer.c#free -->
```c
time_timer_free(t);
```

Go hands a `*Timer` to the collector and forgets about it. There is no collector here, so a timer is one of the objects [design/05-memory.md](../design/05-memory.md) calls out as having a lifetime, and it gets the constructor and destructor pair that go with one.

`time_timer_free` stops the timer and takes it out of whatever heap it is in before it frees the memory. That second part is why this cannot be a plain `mem_free`: a stopped timer is still sitting in a P's heap until that P gets round to throwing it out, and freeing the memory under it would leave the scheduler pointing at a hole. Taking it out costs a walk of that one heap, which is nothing on a timer that has already fired and a few hundred nanoseconds on a P holding a thousand of them.

`NULL` is fine and does nothing, so the usual C cleanup shape works.

What it does not do is wait for a callback that is already running, for the reason in the section above. The callback has its own goroutine and does not touch the timer, so the timer is fine, but anything else that goroutine is holding is your problem.

An arena user can skip all of this. `arena_free` takes the whole region at once and the timers in it go with everything else. [guides/allocators.md](allocators.md) has the rest.

## Timers with a channel

<!-- example: ../examples/time/channels.c#deadline -->
```c
TimeTimer *t = time_new_timer(a, 2 * TIME_SECOND);
Int v;
Time fired;
SelectCase cases[] = {BURROW_RECV(answer, &v),
                      BURROW_RECV(time_timer_c(t), &fired)};
if (chan_select(cases, 2) == 1)
    print_time("gave up at", fired);
time_timer_free(t);
```

`time_new_timer` is `time.NewTimer`. It sends the time on its channel once, when the duration is up, and `time_timer_c` is its `C`. The value is a `Time` and it is the time the timer was due, not the time somebody got round to sending it. A select with a case on it is the usual way to put a deadline on something else.

The channel behaves like Go 1.23's. It reports a length and a capacity of zero, and once `time_timer_stop` or `time_timer_reset` has returned, a receive never sees a value from before the call. A value that was already sitting in the channel is thrown away and counts as the timer having been pending, and a firing that was already on its way on another thread is called off. So the old drain dance, `if !t.Stop() { <-t.C }`, is not needed.

<!-- example: ../examples/time/channels.c#stop -->
```c
TimeTimer *t = time_new_timer(a, TIME_SECOND);
time_sleep(2 * TIME_SECOND);

// It fired a second ago and nobody read the value. Stop throws it away and
// says the timer was stopped in time.
bool stopped = time_timer_stop(t);
Time left;
bool got = chan_try_recv(time_timer_c(t), &left, NULL);
```

`time_timer_free` takes the channel with it, so no goroutine may still be waiting on it.

<!-- example: ../examples/time/channels.c#ticker -->
```c
TimeTicker *tk = time_new_ticker(a, TIME_SECOND);
for (int i = 0; i < 3; i++) {
    Time tick;
    chan_recv(time_ticker_c(tk), &tick);
    print_time("tick", tick);
}
time_ticker_free(tk);
```

`time_new_ticker` is `time.NewTicker`. A tick nobody has read yet stays in the channel, and the ones that come due while it is sitting there are dropped rather than queued. A ticker that falls behind skips what it missed and stays on its original schedule. `time_ticker_stop` stops it without closing the channel, `time_ticker_reset` starts it again with a new period, and `time_ticker_free` gives it back. A period of zero or less panics with Go's message.

<!-- example: ../examples/time/channels.c#after -->
```c
Time fired;
Chan *c = time_after_chan(a, TIME_MINUTE);
chan_recv(c, &fired);
chan_free(c);
```

`time_after_chan` and `time_tick` are `time.After` and `time.Tick`, which hand back only the channel. Go lets the collector have the timer once the channel is unreachable. Here the channel is the handle for both, so `chan_free` on it stops the timer and frees it too. The name has `chan` in it because `time_after` is `Time.After`, the comparison.

All of these have to be called from a goroutine, like `time_after_func`, and answer `NULL` when out of memory.

## Fake time

Inside a [synctest](synctest.md) bubble, everything on this page runs on the bubble's clock rather than the machine's.

That clock starts at midnight UTC on 1 January 2000 and moves only when every goroutine in the bubble is durably blocked, and then it jumps straight to the next timer that is due. So a sleep of an hour in a bubble costs microseconds, an `AfterFunc` armed for a day fires on the next line, and a context deadline thirty seconds out is something a test can wait for rather than something it has to work around.

<!-- example: ../examples/synctest/clock.c#body -->
```c
static void body(void *env) {
    int64_t start = burrow_nanotime();

    time_sleep(TIME_HOUR);

    // Exactly an hour later, on the bubble's clock. The test took no time.
    assert(burrow_nanotime() - start == TIME_HOUR);
}
```

Hand that to `synctest_run` and it gets a bubble of its own.

Nothing on this page had to be told about bubbles for that to work, and neither did `burrow/context.h`. Every timer in the program is armed through one function in the runtime that picks the caller's timer set, and inside a bubble that is the bubble's own set. [guides/synctest.md](synctest.md) has the rules, including the two that catch people out.

## What it costs

A sleeping goroutine is a stack and a timer. The timer is 64 bytes and lives in the heap of the P that armed it, so arming one is a push onto a four way heap under that P's own lock and not a global one. That is the reason this scales: a server that sets a deadline per request has every core pushing onto a different heap, and Go moved off a single global heap in 1.9 after measuring the version that did not.

A thread with nothing to run reads two published words per P to work out how long it can sleep, rather than taking every P's lock, and then sleeps on the monotonic clock until the earliest of them. So an idle program with a timer due in an hour is an idle program, not a program waking up to check.

[design/06-runtime.md](../design/06-runtime.md) has the rest of it, including the three state bits a timer carries and why there are two published minimums rather than one.

## Formatting and parsing

<!-- example: ../examples/time/format.c#format -->
```c
TimeLocation *ist = time_fixed_zone(a, BURROW_S("IST"), 5 * 60 * 60 + 30 * 60);
Time t = time_date(2009, TIME_NOVEMBER, 10, 23, 4, 5, 123456789, time_utc_loc);
Str rfc = time_format(t, a, TIME_RFC3339);
Str there = time_format(time_in(t, ist), a, TIME_RFC1123);
Str own = time_format(t, a, BURROW_S("Mon Jan _2 3:04PM, .000 seconds"));
```

A layout is Go's: the reference time, Mon Jan 2 15:04:05 MST 2006, written the way the output should look. Every number in it is different on purpose, 1 for the month, 2 for the day, 3 or 15 for the hour, 4 for the minute, 5 for the second, 6 or 2006 for the year and -7 for the zone, so a layout reads as a sample of its own output. The pieces are these:

| Element | Means |
|---|---|
| `2006`, `06` | year, four digits or two |
| `January`, `Jan`, `1`, `01` | month by name, short name, number, two digit number |
| `Monday`, `Mon` | weekday |
| `2`, `_2`, `02` | day of the month, space padded or zero padded |
| `__2`, `002` | day of the year |
| `15`, `3`, `03` | hour on a 24 hour clock, on a 12 hour clock, zero padded |
| `4`, `04`, `5`, `05` | minute and second |
| `PM`, `pm` | AM or PM |
| `.000`, `,000`, `.999` | fraction of a second, a fixed number of digits, or with trailing zeros dropped |
| `MST` | zone abbreviation |
| `-0700`, `-07:00`, `-07`, `-070000`, `-07:00:00` | zone offset |
| `Z0700`, `Z07:00`, `Z07`, `Z070000`, `Z07:00:00` | the same, with `Z` for UTC |

Anything else is copied as it is. `TIME_RFC3339`, `TIME_RFC1123`, `TIME_KITCHEN`, `TIME_DATE_ONLY` and the rest are Go's constants under Go's names, and RFC 3339 takes a faster path that skips the layout altogether, as it does in Go. `time_string` is Go's `String`, which is for people reading logs and not for parsing back, and `time_go_string` is what `%#v` prints.

`time_format` returns memory from the allocator you pass, and `time_append_format` appends to a byte slice the way Go's `AppendFormat` does. Both write into a buffer on the stack first, so a result of up to 128 bytes costs one allocation.

<!-- example: ../examples/time/format.c#parse -->
```c
Error err = BURROW_NO_ERROR;
Time p = time_parse(a, TIME_RFC3339, BURROW_S("2006-01-02T15:04:05+07:00"), &err);
Int off = 0;
(void)time_zone(p, &off);
```

`time_parse` reads a value laid out the way the layout says. Parts it leaves out are zero, or 1 for the month and day, and a value with no zone in it is UTC. `time_parse_in_location` is `ParseInLocation`: a value with no zone is in the location you give, and a zone offset or name is matched against that location instead of Local.

A zone offset that Local has at that instant gives back Local. Any other offset gives a fixed zone with no name, which comes from the allocator you pass and lives as long as it does. A whole hour offset comes from a table and allocates nothing. A zone name alone, such as `PST` with no offset next to it, only means something when Local has a zone of that name at the time. Otherwise Go records the name with an offset of zero, and so does this, so parse names only with a layout that carries an offset too.

<!-- example: ../examples/time/format.c#parse-error -->
```c
(void)time_parse(a, TIME_DATE_ONLY, BURROW_S("2024-02-30"), &err);
```

The error is a `TimeParseError` with Go's five fields, and `errors_as` with `TYPE_TIME_PARSE_ERROR` finds it. Its text is Go's to the byte, which matters when something upstream compares it. Errors live in the goroutine's error arena, like every other error here, so a loop that parses many values and throws the errors away should take an `error_mark` and release it.

<!-- example: ../examples/time/format.c#json -->
```c
Slice j = time_marshal_json(t, a, &err);
Time back = {0, 0, NULL};
err = time_unmarshal_json(&back, a, j);
```

The text and JSON encodings are RFC 3339 with as many fraction digits as the time needs. A year outside 0 to 9999 or an offset of 24 hours or more is an error, since RFC 3339 cannot write them. An offset that is not a whole minute loses its seconds, in Go as here. `Time` has a descriptor, `TYPE_TIME`, with these as its methods, so `json_marshal` and `fmt` handle a `Time` without being told.

## What Go has that this does not, yet

The copy of the timezone database that Go can embed, and the Windows registry lookup for Local.

## See also

- [guides/goroutines.md](goroutines.md) for `runtime_main`, `go` and the scheduler these sit on
- [guides/functions.md](functions.md) for `Func`, `BURROW_FN` and where a callback's environment lives
- [guides/allocators.md](allocators.md) for what to pass as `Alloc *` and when the free can be skipped
- [guides/synctest.md](synctest.md) for the bubble's clock and what a test gets out of it
- [design/06-runtime.md](../design/06-runtime.md) for the timer heaps themselves

# Parse, format and add times

Parse an RFC 3339 timestamp, write it out a few other ways, and do some arithmetic on it. A layout is Go's reference time, Mon Jan 2 15:04:05 MST 2006, written the way the output should look. `time_add` adds a fixed duration and `time_add_date` adds calendar months and days. A date that doesn't exist, such as February 30, is an error.

<!-- example: ../examples/cookbook/time_format.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A layout is the reference time, Mon Jan 2 15:04:05 MST 2006, written
     * the way you want yours to look. */
    Error err = NO_ERROR;
    Time t = time_parse(a, TIME_RFC3339, S("2026-03-08T14:30:00-05:00"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    Str s = time_format(t, a, TIME_RFC3339);
    fmt_printf_v("RFC 3339:  %s\n", s);
    s = time_format(t, a, S("Monday, 2 January 2006 at 3:04pm"));
    fmt_printf_v("readable:  %s\n", s);
    s = time_format(time_in(t, time_utc_loc), a, TIME_DATE_TIME);
    fmt_printf_v("in UTC:    %s\n", s);

    /* Add a duration for hours and minutes, and add_date for calendar
     * months, which don't all have the same length. */
    s = time_format(time_add(t, 90 * TIME_MINUTE), a, TIME_KITCHEN);
    fmt_printf_v("+90m:      %s\n", s);
    s = time_format(time_add_date(t, 0, 1, 0), a, TIME_DATE_ONLY);
    fmt_printf_v("+1 month:  %s\n", s);

    Time end = time_parse(a, TIME_RFC3339, S("2026-03-09T09:00:00-05:00"), &err);
    s = duration_string(time_sub(end, t), a);
    fmt_printf_v("until end: %s\n", s);

    /* A date that doesn't exist is an error, not a day in March. */
    time_parse(a, TIME_DATE_ONLY, S("2026-02-30"), &err);
    fmt_println_v(err);
    arena_free(&ar);
}
```

It prints:

```text
RFC 3339:  2026-03-08T14:30:00-05:00
readable:  Sunday, 8 March 2026 at 2:30pm
in UTC:    2026-03-08 19:30:00
+90m:      4:00PM
+1 month:  2026-04-08
until end: 18h30m0s
parsing time "2026-02-30": day out of range
```

The program is [docs/examples/cookbook/time_format.c](../examples/cookbook/time_format.c). CI builds and runs it and checks that it prints this.

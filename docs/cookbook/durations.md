# Parse and print durations

`time_parse_duration` reads strings like `1h30m` or `250ms`, the same form `duration_string` writes, which makes it a handy format for timeouts in a config file. There is no unit for days. `duration_round` and `duration_truncate` make a measured time easier to read.

<!-- example: ../examples/cookbook/durations.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* time_parse_duration reads the same form that duration_string writes,
     * which makes it a good way to put a timeout in a config file. The units
     * are ns, us, ms, s, m and h, and there is no unit for days, since a day
     * is not always 24 hours. */
    static const char *const inputs[] = {"90s", "1h30m", "250ms", "1.5h",
                                         "-2m", "3d",    "10"};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        Error err = NO_ERROR;
        Duration d = time_parse_duration(str_from_cstr(inputs[i]), &err);
        if (FAILED(err)) {
            fmt_println_v(err);
            continue;
        }
        fmt_printf_v("%-6s is %-8s or %g seconds\n", str_from_cstr(inputs[i]),
                     duration_string(d, a), duration_seconds(d));
    }

    /* Rounding makes a measured time easier to read. */
    Duration took =
        1 * TIME_HOUR + 2 * TIME_MINUTE + 3 * TIME_SECOND + 456789 * TIME_MICROSECOND;
    fmt_println_v("took", duration_string(took, a));
    fmt_println_v("about", duration_string(duration_round(took, TIME_SECOND), a));
    fmt_println_v("at least", duration_string(duration_truncate(took, TIME_MINUTE), a));
    arena_free(&ar);
}
```

It prints:

```text
90s    is 1m30s    or 90 seconds
1h30m  is 1h30m0s  or 5400 seconds
250ms  is 250ms    or 0.25 seconds
1.5h   is 1h30m0s  or 5400 seconds
-2m    is -2m0s    or -120 seconds
time: unknown unit "d" in duration "3d"
time: missing unit in duration "10"
took 1h2m3.456789s
about 1h2m3s
at least 1h2m0s
```

The program is [docs/examples/cookbook/durations.c](../examples/cookbook/durations.c). CI builds and runs it and checks that it prints this.

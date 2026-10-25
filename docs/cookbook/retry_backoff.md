# Retry a call that fails for a while

Some failures go away by themselves, such as a busy server, and a second try a little later works. Waiting twice as long after each failure, up to a cap, keeps a client from making things worse. Only retry the errors that a retry can fix: `errors_is` tells a busy server apart from a permission problem.

<!-- example: ../examples/cookbook/retry_backoff.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

BURROW_SENTINEL_ERROR(err_busy, "server busy");
BURROW_SENTINEL_ERROR(err_denied, "permission denied");

/* Stands in for a call to a service. It fails with err_busy until it has been
 * called fail_times times, or with err_denied every time if denied is set. */
typedef struct Service {
    Int calls;
    Int fail_times;
    bool denied;
} Service;

static Error call_service(Service *s) {
    s->calls++;
    if (s->denied)
        return err_denied;
    if (s->calls <= s->fail_times)
        return err_busy;
    return NO_ERROR;
}

/* Calls the service up to tries times, waiting twice as long after each
 * failure, up to a cap. Only a busy server is worth another go: trying again
 * won't fix a permission problem. A real client would add some randomness to
 * each wait, so that clients that failed together don't retry together. */
static Error with_retries(Alloc *a, Service *s, Int tries) {
    Duration wait = 10 * TIME_MILLISECOND;
    const Duration most = 40 * TIME_MILLISECOND;
    Error err = NO_ERROR;
    for (Int i = 1; i <= tries; i++) {
        err = call_service(s);
        if (OK(err) || !errors_is(err, err_busy))
            return err;
        if (i == tries)
            break;
        fmt_printf_v("try %d: %v, waiting %s\n", i, err, duration_string(wait, a));
        time_sleep(wait);
        wait *= 2;
        if (wait > most)
            wait = most;
    }
    return fmt_errorf_v("gave up after %d tries: %w", tries, err);
}

static void report(Error err) {
    if (FAILED(err))
        fmt_println_v("error:", err);
    else
        fmt_println_v("done");
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Service flaky = {0, 3, false};
    report(with_retries(a, &flaky, 6));

    Service down = {0, 100, false};
    report(with_retries(a, &down, 3));

    Service locked = {0, 0, true};
    report(with_retries(a, &locked, 6));
    fmt_println_v("calls to the locked service:", locked.calls);
    arena_free(&ar);
}
```

It prints:

```text
try 1: server busy, waiting 10ms
try 2: server busy, waiting 20ms
try 3: server busy, waiting 40ms
done
try 1: server busy, waiting 10ms
try 2: server busy, waiting 20ms
error: gave up after 3 tries: server busy
error: permission denied
calls to the locked service: 1
```

The program is [docs/examples/cookbook/retry_backoff.c](../examples/cookbook/retry_backoff.c). CI builds and runs it and checks that it prints this.

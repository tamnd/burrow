#include <stdio.h>

#include "burrow/burrow.h"

// A lookup that takes three seconds to answer.
static void slow_lookup(void *env) {
    Chan *answer = env;
    time_sleep(3 * TIME_SECOND);
    Int v = 42;
    chan_send(answer, &v);
}

static void print_time(const char *what, Time v) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str s = time_format(time_utc(v), arena_allocator(&ar), TIME_TIME_ONLY);
    printf("%s %.*s\n", what, (int)s.len, (const char *)s.p);
    arena_free(&ar);
}

static void deadline(Alloc *a) {
    Chan *answer = chan_make(a, TYPE_INT, 1);
    (void)go(BURROW_FN(Func, slow_lookup, answer));

    // doc: deadline
    TimeTimer *t = time_new_timer(a, 2 * TIME_SECOND);
    Int v;
    Time fired;
    SelectCase cases[] = {BURROW_RECV(answer, &v),
                          BURROW_RECV(time_timer_c(t), &fired)};
    if (chan_select(cases, 2) == 1)
        print_time("gave up at", fired);
    time_timer_free(t);
    // doc: end

    // The lookup still finishes, into a channel with room for it.
    time_sleep(2 * TIME_SECOND);
    chan_free(answer);
}

static void ticker(Alloc *a) {
    // doc: ticker
    TimeTicker *tk = time_new_ticker(a, TIME_SECOND);
    for (int i = 0; i < 3; i++) {
        Time tick;
        chan_recv(time_ticker_c(tk), &tick);
        print_time("tick", tick);
    }
    time_ticker_free(tk);
    // doc: end
}

static void stop(Alloc *a) {
    // doc: stop
    TimeTimer *t = time_new_timer(a, TIME_SECOND);
    time_sleep(2 * TIME_SECOND);

    // It fired a second ago and nobody read the value. Stop throws it away and
    // says the timer was stopped in time.
    bool stopped = time_timer_stop(t);
    Time left;
    bool got = chan_try_recv(time_timer_c(t), &left, NULL);
    // doc: end
    printf("stopped: %s, value left: %s\n", stopped ? "yes" : "no", got ? "yes" : "no");
    time_timer_free(t);
}

static void after(Alloc *a) {
    // doc: after
    Time fired;
    Chan *c = time_after_chan(a, TIME_MINUTE);
    chan_recv(c, &fired);
    chan_free(c);
    // doc: end
    print_time("after a minute", fired);
}

static void body(void *env) {
    Alloc *a = heap_allocator();
    deadline(a);
    ticker(a);
    stop(a);
    after(a);
}

static void top(void *env) {
    synctest_run(BURROW_FN(Func, body, NULL));
}

int main(void) {
    runtime_main(BURROW_FN(Func, top, NULL));
    return 0;
}

/* Output:
gave up at 00:00:02
tick 00:00:05
tick 00:00:06
tick 00:00:07
stopped: yes, value left: no
after a minute 00:01:09
*/

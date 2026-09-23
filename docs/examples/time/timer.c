#include <stdio.h>

#include "burrow/burrow.h"

// A connection that takes a while to send its request, and prints when it is
// closed on it.
typedef struct Conn {
    const char *name;
    Duration takes;
} Conn;

static void conn_close(void *env) {
    Conn *c = env;
    printf("%s: gave up waiting\n", c->name);
}

static void conn_read_request(Conn *c) {
    time_sleep(c->takes);
    printf("%s: request read\n", c->name);
}

// doc: after
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
// doc: end

static void noop(void *env) {}

static Error stop_and_reset(Alloc *a) {
    TimeTimer *t = time_after_func(a, TIME_MINUTE, BURROW_FN(Func, noop, NULL));
    if (t == NULL)
        return burrow_err_out_of_memory;

    // doc: stop
    bool was_waiting = time_timer_stop(t);
    // doc: end
    printf("stopped while waiting: %s\n", was_waiting ? "yes" : "no");

    // doc: reset
    bool pending;
    if (!time_timer_reset(t, 5 * TIME_SECOND, &pending))
        return burrow_err_out_of_memory;
    // doc: end
    printf("reset while waiting: %s\n", pending ? "yes" : "no");

    time_sleep(10 * TIME_SECOND);
    printf("stopped after it fired: %s\n", time_timer_stop(t) ? "no" : "yes");

    // doc: free
    time_timer_free(t);
    // doc: end
    return BURROW_NO_ERROR;
}

static void body(void *env) {
    Alloc *a = heap_allocator();
    Conn quick = {"quick", TIME_SECOND};
    Conn slow = {"slow", 10 * TIME_SECOND};
    Error err = serve(a, &quick);
    if (BURROW_OK(err))
        err = serve(a, &slow);
    if (BURROW_OK(err))
        err = stop_and_reset(a);
    printf("failed: %s\n", BURROW_FAILED(err) ? "yes" : "no");
}

static void top(void *env) {
    synctest_run(BURROW_FN(Func, body, NULL));
}

int main(void) {
    runtime_main(BURROW_FN(Func, top, NULL));
    return 0;
}

/* Output:
quick: request read
slow: gave up waiting
slow: request read
stopped while waiting: yes
reset while waiting: no
stopped after it fired: yes
failed: no
*/

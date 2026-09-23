#include <stdio.h>

#include "burrow/burrow.h"

// doc: chan
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
// doc: end

static void do_the_job(Int job) {
    printf("job %lld\n", (long long)job);
}

static void obey(Str msg) {
    printf("told to " BURROW_STR_FMT "\n", BURROW_STR_ARG(msg));
}

static void nothing_ready(void) {
    printf("nothing ready\n");
}

static void selecting(Chan *work, Chan *control) {
    // clang-format off
    // doc: select
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
    // doc: end
    // clang-format on
}

typedef struct Conn {
    bool abandoned;
} Conn;

static void give_up(void *env) {
    Conn *c = env;
    c->abandoned = true;
}

static void talk_to(Conn *c) {
    (void)c;
}

static void timers(Alloc *a) {
    Conn the_conn = {false};
    Conn *conn = &the_conn;

    // doc: timers
    time_sleep(500 * TIME_MILLISECOND);

    TimeTimer *t = time_after_func(a, 5 * TIME_SECOND, BURROW_FN(Func, give_up, conn));
    talk_to(conn);
    time_timer_stop(t);
    time_timer_free(t);
    // doc: end
    printf("gave up on the connection %d\n", conn->abandoned);
}

static void run(void *env) {
    Arena ar;
    (void)env;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    printf("sum %lld\n", (long long)sum_all(a));

    Chan *work = chan_make(a, TYPE_INT, 1);
    Chan *control = chan_make(a, TYPE_STRING, 1);
    selecting(work, control);
    Int j = 3;
    chan_send(work, &j);
    selecting(work, control);
    Str stop = BURROW_S("stop");
    chan_send(control, &stop);
    selecting(work, control);
    chan_free(work);
    chan_free(control);

    timers(a);
    arena_free(&ar);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
sum 45
nothing ready
job 3
told to stop
gave up on the connection 0
*/

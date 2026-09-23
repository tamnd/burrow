#include <stdio.h>

#include "burrow/burrow.h"

static Int dropped;

static void drop(Int job) {
    dropped++;
}

static void do_the_job(Int job) {
    printf("job %lld\n", (long long)job);
}

static void obey(Str msg) {
    printf("told to %.*s\n", (int)msg.len, (const char *)msg.p);
}

// Select picks among ready arms at random, as Go's does, so the order the
// values arrive in changes from run to run and only the total is printed.
static Int total;

static void use(Int v) {
    total += v;
}

static void offer(Chan *work, Int job) {
    // doc: try
    if (!chan_try_send(work, &job)) {
        /* the queue is full, shed the load rather than blocking on it */
        drop(job);
    }
    // doc: end
}

static void serve_one(Chan *work, Chan *control) {
    // doc: select
    Int job;
    Str msg;

    SelectCase cases[] = {
        BURROW_RECV(work, &job),
        BURROW_RECV(control, &msg),
    };

    switch (chan_select(cases, 2)) {
    case 0:
        do_the_job(job);
        break;
    case 1:
        obey(msg);
        break;
    default:
        break;
    }
    // doc: end
}

// doc: merge
static void merge(Chan *a, Chan *b) {
    while (a != NULL || b != NULL) {
        Int v;
        bool ok;
        SelectCase cases[] = {
            BURROW_RECV_OK(a, &v, &ok),
            BURROW_RECV_OK(b, &v, &ok),
        };

        Int arm = chan_select(cases, 2);
        if (!ok) {
            /* that one is closed and drained, so stop listening to it */
            if (arm == 0)
                a = NULL;
            else
                b = NULL;
            continue;
        }
        use(v);
    }
}
// doc: end

static void run(void *env) {
    Alloc *h = heap_allocator();

    Chan *work = chan_make(h, TYPE_INT, 2);
    for (Int job = 1; job <= 5; job++)
        offer(work, job);
    printf("queued %lld, dropped %lld\n", (long long)chan_len(work),
           (long long)dropped);

    Chan *control = chan_make(h, TYPE_STRING, 1);
    serve_one(work, control);
    serve_one(work, control);
    Str stop = BURROW_S("stop");
    chan_send(control, &stop);
    serve_one(work, control);

    Chan *a = chan_make(h, TYPE_INT, 2);
    Chan *b = chan_make(h, TYPE_INT, 1);
    Int one = 1, two = 2, three = 3;
    chan_send(a, &one);
    chan_send(a, &two);
    chan_send(b, &three);
    chan_close(a);
    chan_close(b);
    merge(a, b);
    printf("both closed, total %lld\n", (long long)total);

    chan_free(work);
    chan_free(control);
    chan_free(a);
    chan_free(b);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
queued 2, dropped 3
job 1
job 2
told to stop
both closed, total 6
*/

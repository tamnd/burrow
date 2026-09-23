#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/sync.h"
#include "burrow/time.h"

/* The same prototypes burrow/context.h declares. Repeating one is legal C and
 * the compiler rejects the pair if they ever disagree. */
// doc: questions
bool context_deadline(Context c, int64_t *when);
Chan *context_done(Context c);
Error context_err(Context c);
Any context_value(Context c, Any key);
// doc: end

static SyncWaitGroup finished;
/* One slice of whatever the loop is for. */
static void step(void) {
    runtime_gosched();
}

// doc: poll
static void work(void *env) {
    Context ctx = *(Context *)env;
    bool ok;

    while (!chan_try_recv(context_done(ctx), NULL, &ok))
        step();
    sync_wait_group_done(&finished);
}
// doc: end

static Error start_and_stop(Alloc *a) {
    sync_wait_group_add(&finished, 1);

    // doc: cancel
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);
    if (BURROW_CONTEXT_IS_NIL(ctx))
        return burrow_err_out_of_memory;

    go(BURROW_FN(Func, work, &ctx));
    runtime_gosched(); /* give it a moment to get going */

    BURROW_CALLF0(cancel);
    sync_wait_group_wait(&finished); /* work has seen it and returned */
    context_free(ctx);
    // doc: end
    printf("work stopped\n");
    return BURROW_NO_ERROR;
}

typedef struct Server {
    Context ctx;
    Chan *jobs;
} Server;

static Int total;

static void do_the_job(const Int *job) {
    total += *job;
}

static void serve(void *env) {
    Server *s = env;
    Context ctx = s->ctx;
    Chan *jobs = s->jobs;
    Int job;

    for (;;) {
        // doc: select
        SelectCase cases[2];

        cases[0] = BURROW_RECV(context_done(ctx), NULL);
        cases[1] = BURROW_RECV(jobs, &job);

        if (chan_select(cases, 2) == 0)
            break;
        do_the_job(&job);
        // doc: end
    }
    sync_wait_group_done(&finished);
}

static void serve_until_cancelled(Alloc *a) {
    ContextCancelFunc cancel;
    Server s = {context_with_cancel(a, context_background(), &cancel),
                chan_make(a, TYPE_INT, 0)};

    sync_wait_group_add(&finished, 1);
    go(BURROW_FN(Func, serve, &s));
    for (Int i = 1; i <= 3; i++)
        chan_send(s.jobs, &i);
    BURROW_CALLF0(cancel);
    sync_wait_group_wait(&finished);
    printf("served jobs adding up to %lld\n", (long long)total);
    context_free(s.ctx);
    chan_free(s.jobs);
}

static Error talk_to_the_database(Context ctx) {
    return context_err(ctx); /* a quick query, done well inside the deadline */
}

static Error query(Alloc *a, Context parent) {
    // doc: timeout
    ContextCancelFunc cancel;
    Context ctx = context_with_timeout(a, parent, 5 * TIME_SECOND, &cancel);
    if (BURROW_CONTEXT_IS_NIL(ctx))
        return burrow_err_out_of_memory;

    Error err = talk_to_the_database(ctx);

    BURROW_CALLF0(cancel);
    context_free(ctx);
    // doc: end
    return err;
}

static void log_error(Error err) {
    printf("error: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_message(err)));
}

// doc: cause
BURROW_SENTINEL_ERROR(err_client_hung_up, "the client hung up");

static void handle(Alloc *a, Context parent) {
    ContextCancelCauseFunc cancel;
    Context ctx = context_with_cancel_cause(a, parent, &cancel);
    if (BURROW_CONTEXT_IS_NIL(ctx))
        return;

    /* Somewhere further up, the connection closes. */
    BURROW_CALLF(cancel, err_client_hung_up);

    /* And somewhere further down, the work notices. */
    if (BURROW_FAILED(context_err(ctx)))
        log_error(context_cause(ctx)); /* the client hung up */
    context_free(ctx);
}
// doc: end

BURROW_SENTINEL_ERROR(err_the_database_is_slow, "the database is slow");

static void slow(Alloc *a, Context parent) {
    ContextCancelFunc cancel;

    // doc: timeout-cause
    Context ctx = context_with_timeout_cause(a, parent, 5 * TIME_SECOND,
                                             err_the_database_is_slow, &cancel);
    // doc: end
    if (BURROW_CONTEXT_IS_NIL(ctx))
        return;
    BURROW_CALLF0(cancel);
    printf("cancelled first, so the cause is: " BURROW_STR_FMT "\n",
           BURROW_STR_ARG(error_message(context_cause(ctx))));
    context_free(ctx);
}

static void expire(Alloc *a) {
    ContextCancelFunc cancel;
    Context ctx =
        context_with_timeout_cause(a, context_background(), 10 * TIME_MILLISECOND,
                                   err_the_database_is_slow, &cancel);
    if (BURROW_CONTEXT_IS_NIL(ctx))
        return;
    chan_recv(context_done(ctx), NULL);
    printf("deadline went by: err " BURROW_STR_FMT ", cause " BURROW_STR_FMT "\n",
           BURROW_STR_ARG(error_message(context_err(ctx))),
           BURROW_STR_ARG(error_message(context_cause(ctx))));
    BURROW_CALLF0(cancel);
    context_free(ctx);
}

// doc: values
/* In your own .c file, and nowhere else. */
#define REQUEST_ID_KEY_FIELDS(F, T) F(T, Int, n, "")
BURROW_STRUCT(RequestIDKey, REQUEST_ID_KEY_FIELDS);

static RequestIDKey request_id_key;

#define REQUEST_ID BURROW_ANY(TYPE_OF(RequestIDKey), &request_id_key)
// doc: end

static void log_with_id(Int id) {
    printf("request %lld\n", (long long)id);
}

static void values(Alloc *a, Context parent) {
    Int id = 7;

    // doc: with-value
    Context ctx = context_with_value(a, parent, REQUEST_ID, BURROW_ANY(TYPE_INT, &id));

    Any v = context_value(ctx, REQUEST_ID);
    if (!BURROW_ANY_IS_NIL(v))
        log_with_id(*(Int *)v.data);
    // doc: end

    // doc: free
    context_free(ctx);
    // doc: end
}

static void write_the_audit_log(void *env) {
    Context *ctx = env;
    Any v = context_value(*ctx, REQUEST_ID);

    printf("audit log for request %lld, still live %d\n", (long long)*(Int *)v.data,
           BURROW_OK(context_err(*ctx)));
    sync_wait_group_done(&finished);
}

static void detach(Alloc *a) {
    Int id = 8;
    ContextCancelFunc cancel;
    Context req = context_with_cancel(a, context_background(), &cancel);
    Context ctx = context_with_value(a, req, REQUEST_ID, BURROW_ANY(TYPE_INT, &id));

    BURROW_CALLF0(cancel); /* the response has gone out */
    sync_wait_group_add(&finished, 1);

    // doc: detached
    Context *detached = BURROW_NEW(a, Context);
    *detached = context_without_cancel(a, ctx);

    go(BURROW_FN(Func, write_the_audit_log, detached));
    // doc: end

    sync_wait_group_wait(&finished);
    context_free(*detached);
    mem_free(a, detached, sizeof *detached, _Alignof(Context));
    context_free(ctx);
    context_free(req);
}

typedef struct Conn {
    int fd;
} Conn;

static void close_the_connection(void *env) {
    Conn *c = env;
    printf("closing %d\n", c->fd);
}

static void serve_the_connection(Conn *c) {
    printf("serving %d\n", c->fd);
}

static Error after(Alloc *a, Context ctx) {
    Conn conn = {3};
    Conn *c = &conn;

    // doc: after
    StopFunc stop;
    Context reg =
        context_after_func(a, ctx, BURROW_FN(Func, close_the_connection, c), &stop);
    if (BURROW_CONTEXT_IS_NIL(reg))
        return burrow_err_out_of_memory;

    serve_the_connection(c);

    (void)BURROW_CALLF0(stop);
    context_free(reg);
    // doc: end
    return BURROW_NO_ERROR;
}

/* A context of our own: never cancelled, carrying one value, and forwarding
 * every other key to the context it wraps. */
typedef struct MyState {
    Context parent;
    Int answer;
} MyState;

static bool my_deadline(void *self, int64_t *when) {
    (void)self;
    (void)when;
    return false;
}

static Chan *my_done(void *self) {
    (void)self;
    return NULL;
}

static Error my_err(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static Any my_value(void *self, Any key) {
    MyState *s = self;
    if (key.t == TYPE_INT)
        return BURROW_ANY(TYPE_INT, &s->answer);
    return context_value(s->parent, key);
}

static MyState my_state = {{NULL, NULL}, 42};

// doc: custom
static const ContextVT my_vt = {NULL, my_deadline, my_done, my_err, my_value};

Context c = {&my_vt, &my_state};
// doc: end

static void custom(Alloc *a) {
    Int id = 9;
    Context parent = context_with_value(a, context_background(), REQUEST_ID,
                                        BURROW_ANY(TYPE_INT, &id));
    my_state.parent = parent;

    Int zero = 0;
    Any mine = context_value(c, BURROW_ANY(TYPE_INT, &zero));
    Any theirs = context_value(c, REQUEST_ID);

    ContextCancelFunc cancel;
    Context child = context_with_cancel(a, c, &cancel);
    BURROW_CALLF0(cancel);
    chan_recv(context_done(child), NULL);
    printf("custom: answer %lld, request %lld, child cancelled %d\n",
           (long long)*(Int *)mine.data, (long long)*(Int *)theirs.data,
           BURROW_FAILED(context_err(child)));
    context_free(child);
    context_free(parent);
}

static void run(void *env) {
    (void)env;
    Alloc *a = heap_allocator();

    start_and_stop(a);
    serve_until_cancelled(a);
    Error err = query(a, context_background());
    printf("query ok %d\n", BURROW_OK(err));
    handle(a, context_background());
    slow(a, context_background());
    expire(a);
    values(a, context_background());
    detach(a);
    after(a, context_background());
    custom(a);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
work stopped
served jobs adding up to 6
query ok 1
error: the client hung up
cancelled first, so the cause is: context canceled
deadline went by: err context deadline exceeded, cause the database is slow
request 7
audit log for request 8, still live 1
serving 3
custom: answer 42, request 9, child cancelled 1
*/

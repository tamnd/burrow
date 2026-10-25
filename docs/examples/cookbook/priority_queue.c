// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/container/heap.h"

#define JOB_FIELDS(F, T)                                                               \
    F(T, Str, Name, "")                                                                \
    F(T, Int, Due, "")                                                                 \
    F(T, Int, Index, "")
BURROW_STRUCT(Job, JOB_FIELDS);

/* The jobs in heap order, earliest due first. container/heap does the
 * ordering and the queue only has to say how to compare, swap, add and take
 * away. Each job keeps its own index up to date so heap_fix can find it. */
typedef struct Queue {
    Job *jobs[16];
    Int n;
} Queue;

static Int queue_len(void *self) {
    return ((Queue *)self)->n;
}

static bool queue_less(void *self, Int i, Int j) {
    Queue *q = self;
    return q->jobs[i]->Due < q->jobs[j]->Due;
}

static void queue_swap(void *self, Int i, Int j) {
    Queue *q = self;
    Job *t = q->jobs[i];
    q->jobs[i] = q->jobs[j];
    q->jobs[j] = t;
    q->jobs[i]->Index = i;
    q->jobs[j]->Index = j;
}

/* Called by heap_push to add x at the end, before it moves into place. */
static void queue_push(void *self, Any x) {
    Queue *q = self;
    Job *job = x.data;
    job->Index = q->n;
    q->jobs[q->n++] = job;
}

/* Called by heap_pop once the earliest job has been swapped to the end. */
static Any queue_pop(void *self) {
    Queue *q = self;
    Job *job = q->jobs[--q->n];
    job->Index = -1;
    return ANY(TYPE_OF(Job), job);
}

static const HeapInterfaceVT queue_vt = {
    {NULL, queue_len, queue_less, queue_swap}, queue_push, queue_pop};

static Job *add(Alloc *a, HeapInterface h, const char *name, Int due) {
    Job *job = BURROW_NEW(a, Job);
    job->Name = str_from_cstr(name);
    job->Due = due;
    heap_push(h, ANY(TYPE_OF(Job), job));
    return job;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Queue q = {{NULL}, 0};
    HeapInterface h = {&queue_vt, &q};
    add(a, h, "backup", 30);
    add(a, h, "send report", 10);
    Job *cleanup = add(a, h, "clean up", 50);
    add(a, h, "rotate logs", 20);

    /* Moving a job's time means fixing its place in the heap. */
    cleanup->Due = 5;
    heap_fix(h, cleanup->Index);

    while (q.n > 0) {
        Job *job = heap_pop(h).data;
        fmt_printf_v("%2d %s\n", job->Due, job->Name);
        /* A job can add more work, and it goes in at the right place. */
        if (str_eq(job->Name, S("backup")))
            add(a, h, "check backup", job->Due + 5);
    }
    arena_free(&ar);
}
// doc: end

/* Output:
 5 clean up
10 send report
20 rotate logs
30 backup
35 check backup
*/

#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

// doc: implement
#define COUNTER_FIELDS(F, T) F(T, Int, n, "")
BURROW_STRUCT(Counter, COUNTER_FIELDS);

static Int counter_read(void *self, Slice p, Error *err) {
    Counter *c = (Counter *)self;
    Int i = 0;

    for (; i < p.len && c->n < 10; i++)
        BURROW_AT(Byte, p, i) = (Byte)('0' + c->n++);
    if (i == 0)
        *err = io_eof;
    return i;
}

static const IoReaderVT counter_reader_vt = {TYPE_OF(Counter), counter_read};

IoReader counter_as_io_reader(Counter *c) {
    IoReader r = {&counter_reader_vt, c};
    return r;
}
// doc: end

/* A pipe with a small buffer: what is written can be read back. */
#define PIPE_FIELDS(F, T)                                                              \
    F(T, Int, len, "")                                                                 \
    F(T, Int, off, "")
BURROW_STRUCT(Pipe, PIPE_FIELDS);

static Byte pipe_buf[64];

static Int pipe_read(void *self, Slice p, Error *err) {
    Pipe *q = self;
    Int n = q->len - q->off < p.len ? q->len - q->off : p.len;
    if (n == 0) {
        *err = io_eof;
        return 0;
    }
    memcpy(p.p, pipe_buf + q->off, (size_t)n);
    q->off += n;
    return n;
}

static Int pipe_write(void *self, Slice p, Error *err) {
    Pipe *q = self;
    (void)err;
    memcpy(pipe_buf + q->len, p.p, (size_t)p.len);
    q->len += p.len;
    return p.len;
}

// doc: pipe
static const IoReadWriterVT pipe_read_writer_vt = {
    {TYPE_OF(Pipe), pipe_read},
    {TYPE_OF(Pipe), pipe_write},
};
// doc: end

static void call(void) {
    Counter counter = {0};
    IoReader r = counter_as_io_reader(&counter);
    Byte bytes[4];
    Slice buf = slice_from(bytes, 4, 4, TYPE_BYTE);

    // doc: call
    Error err = BURROW_NO_ERROR;
    Int n = BURROW_CALL(r, read, buf, &err);
    // doc: end
    printf("read %lld: %.*s\n", (long long)n, (int)n, (const char *)bytes);

    // doc: assert
    Counter *c = iface_assert(BURROW_IFACE(r), TYPE_OF(Counter));
    if (c != NULL) {
        /* it really is a counter, so the fast path is available */
    }
    // doc: end
    printf("asserted a counter at %lld\n", (long long)c->n);
}

static void nil(void) {
    // doc: nil
    IoReader r = {NULL, NULL};
    if (BURROW_IFACE_IS_NIL(r)) {
        printf("r is nil\n");
    }
    // doc: end
}

static void narrow(void) {
    Pipe pipe = {0, 0};
    IoReadWriter rw = {&pipe_read_writer_vt, &pipe};

    // doc: narrow
    IoReader r = io_read_writer_as_io_reader(rw);
    IoWriter w = io_read_writer_as_io_writer(rw);
    // doc: end

    Error err = BURROW_NO_ERROR;
    BURROW_CALL(w, write, slice_from((void *)"through the pipe", 16, 16, TYPE_BYTE),
                &err);
    Byte got[32];
    Int n = BURROW_CALL(r, read, slice_from(got, 32, 32, TYPE_BYTE), &err);
    printf("%.*s\n", (int)n, (const char *)got);
}

static void any(Alloc *a) {
    // doc: any
    Int n = 42;
    Any a1 = BURROW_ANY(TYPE_INT, &n);
    Any a2 = BURROW_ANY_VAL(TYPE_INT, Int, 42);
    // doc: end

    // doc: box
    Any kept = any_box(a, a1);
    // doc: end
    n = 7;

    Any v = kept;
    // doc: any-assert
    Int *got = (Int *)any_assert(v, TYPE_INT);
    // doc: end
    printf("kept %lld while n is %lld, a1 equals a2 %d\n", (long long)*got,
           (long long)n, any_equal(kept, a2));
}

static void compare(void) {
    // doc: equal
    Int i = 3;
    int64_t j = 3;
    any_equal(BURROW_ANY(TYPE_INT, &i), BURROW_ANY(TYPE_INT64, &j)); /* false */
    // doc: end
    printf("int 3 equals int64 3 %d\n",
           any_equal(BURROW_ANY(TYPE_INT, &i), BURROW_ANY(TYPE_INT64, &j)));
}

int main(void) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);

    call();
    nil();
    narrow();
    any(arena_allocator(&ar));
    compare();
    arena_free(&ar);
    return 0;
}

/* Output:
read 4: 0123
asserted a counter at 4
r is nil
through the pipe
kept 42 while n is 7, a1 equals a2 1
int 3 equals int64 3 0
*/

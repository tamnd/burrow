#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

static void print_bytes(const char *label, Slice b) {
    printf("%s: %.*s\n", label, (int)b.len, (const char *)b.p);
}

// doc: producer
static void produce(void *env) {
    IoPipeWriter *w = (IoPipeWriter *)env;
    io_write_string(io_pipe_writer_as_io_writer(w), BURROW_S("sent through a pipe"),
                    NULL);
    io_pipe_writer_close(w);
}
// doc: end

static void run(void *env) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: copy
    StringsReader src;
    strings_reader_reset(&src, BURROW_S("hello, world"));
    BytesBuffer dst = BYTES_BUFFER(a);
    Error err = BURROW_NO_ERROR;
    int64_t n = io_copy(a, bytes_buffer_as_io_writer(&dst),
                        strings_reader_as_io_reader(&src), &err);
    // doc: end
    printf("copied %lld bytes\n", (long long)n);
    print_bytes("dst", bytes_buffer_bytes(&dst));

    // doc: readall
    strings_reader_reset(&src, BURROW_S("the quick brown fox"));
    IoLimitedReader lim = io_limit_reader(strings_reader_as_io_reader(&src), 9);
    Slice head = io_read_all(a, io_limited_reader_as_io_reader(&lim), &err);
    // doc: end
    print_bytes("limited", head);

    // doc: section
    strings_reader_reset(&src, BURROW_S("0123456789"));
    IoSectionReader sec =
        io_new_section_reader(strings_reader_as_io_reader_at(&src), 3, 4);
    Slice mid = io_read_all(a, io_section_reader_as_io_reader(&sec), &err);
    // doc: end
    print_bytes("section", mid);
    printf("section size: %lld\n", (long long)io_section_reader_size(&sec));

    // doc: tee
    strings_reader_reset(&src, BURROW_S("seen twice"));
    BytesBuffer copy = BYTES_BUFFER(a);
    IoTeeReader tee = io_tee_reader(strings_reader_as_io_reader(&src),
                                    bytes_buffer_as_io_writer(&copy));
    Slice read = io_read_all(a, io_tee_reader_as_io_reader(&tee), &err);
    // doc: end
    print_bytes("read", read);
    print_bytes("tee", bytes_buffer_bytes(&copy));

    // doc: multi
    StringsReader r1, r2;
    strings_reader_reset(&r1, BURROW_S("one, "));
    strings_reader_reset(&r2, BURROW_S("two"));
    IoReader parts[2] = {strings_reader_as_io_reader(&r1),
                         strings_reader_as_io_reader(&r2)};
    IoReader both = io_multi_reader(a, parts, 2);

    BytesBuffer w1 = BYTES_BUFFER(a), w2 = BYTES_BUFFER(a);
    IoWriter sinks[2] = {bytes_buffer_as_io_writer(&w1),
                         bytes_buffer_as_io_writer(&w2)};
    IoWriter fan = io_multi_writer(a, sinks, 2);
    io_copy(a, fan, both, &err);
    // doc: end
    print_bytes("w1", bytes_buffer_bytes(&w1));
    print_bytes("w2", bytes_buffer_bytes(&w2));

    // doc: pipe
    IoPipeReader *pr;
    IoPipeWriter *pw;
    io_pipe(a, &pr, &pw);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, produce, pw));
    Slice got = io_read_all(a, io_pipe_reader_as_io_reader(pr), &err);
    sync_wait_group_wait(&wg);
    io_pipe_free(pr);
    // doc: end
    print_bytes("pipe", got);

    // doc: errors
    strings_reader_reset(&src, BURROW_S("short"));
    Byte buf[8];
    Int got_n = io_read_full(strings_reader_as_io_reader(&src),
                             slice_from(buf, 8, 8, TYPE_BYTE), &err);
    // doc: end
    printf("read_full: %lld, " BURROW_STR_FMT "\n", (long long)got_n,
           BURROW_STR_ARG(error_text(err)));

    arena_free(&ar);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
copied 12 bytes
dst: hello, world
limited: the quick
section: 3456
section size: 4
read: seen twice
tee: seen twice
w1: one, two
w2: one, two
pipe: sent through a pipe
read_full: 5, unexpected EOF
*/

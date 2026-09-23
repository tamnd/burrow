#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

/* The same prototypes burrow/error.h declares. */
// doc: walk
Error errors_unwrap(Error err);
bool errors_is(Error err, Error target);
const void *errors_as(Error err, const Type *target);
// doc: end

BURROW_SENTINEL_ERROR(err_cannot_open, "cannot open the file");

static Error save_config(Str path, Str data) {
    char name[256];
    snprintf(name, sizeof name, BURROW_STR_FMT, BURROW_STR_ARG(path));
    FILE *f = fopen(name, "w");
    if (f == NULL)
        return err_cannot_open;
    fwrite(data.p, 1, (size_t)data.len, f);
    fclose(f);
    return BURROW_NO_ERROR;
}

static void check(void) {
    Str path = BURROW_S("no/such/dir/config.txt");
    Str data = BURROW_S("verbose = true\n");

    // doc: check
    Error err = save_config(path, data);
    if (BURROW_FAILED(err))
        printf("nope: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
    // doc: end

    err = save_config(BURROW_S("config.txt"), data);

    // doc: failed
    if (BURROW_FAILED(err)) {
        printf("failed\n");
    }
    if (BURROW_OK(err)) {
        printf("saved\n");
    }
    // doc: end

    // doc: message
    Str msg = error_text(err);
    // doc: end
    printf("message of no error is %lld bytes\n", (long long)msg.len);
}

// doc: sentinel
/* in the .c file */
BURROW_SENTINEL_ERROR(err_not_found, "record not found");

/* in the header */
extern const Error err_not_found;
// doc: end

static void make(Alloc *a) {
    // doc: new
    Error err = errors_new(a, BURROW_S("record not found"));
    // doc: end
    printf("errors_is the sentinel with the same text: %d\n",
           errors_is(err, err_not_found));
}

static Error next(Int *left) {
    if (*left == 0)
        return io_eof;
    (*left)--;
    return BURROW_NO_ERROR;
}

static void read_all(void) {
    Int left = 3, read = 0;

    for (;;) {
        Error err = next(&left);

        // doc: is
        if (errors_is(err, io_eof))
            break;
        // doc: end
        read++;
    }
    printf("read %lld, then EOF\n", (long long)read);
}

#define PARSE_ERROR_FIELDS(F, T)                                                       \
    F(T, Int, line, "")                                                                \
    F(T, Str, text, "")
BURROW_STRUCT(ParseError, PARSE_ERROR_FIELDS);

static Str parse_message(const void *self) {
    return ((const ParseError *)self)->text;
}

static const ErrorVT parse_vt = {
    TYPE_OF(ParseError), parse_message, NULL, NULL, NULL, NULL, NULL,
};

// doc: custom
typedef struct MyError {
    Str text;
    Error cause;
} MyError;

static Str my_message(const void *self) {
    return ((const MyError *)self)->text;
}
static Error my_unwrap(const void *self) {
    return ((const MyError *)self)->cause;
}

static const ErrorVT my_vt = {
    NULL, my_message, my_unwrap, NULL, NULL, NULL, NULL,
};
// doc: end

static void extract(void) {
    static const ParseError bad = {12, BURROW_S_INIT("unexpected comma")};
    static const MyError wrapped = {BURROW_S_INIT("loading config: unexpected comma"),
                                    {&parse_vt, &bad}};
    Error err = {&my_vt, &wrapped};

    // doc: as
    const ParseError *pe = errors_as(err, TYPE_OF(ParseError));
    if (pe != NULL)
        printf("failed on line %lld\n", (long long)pe->line);
    // doc: end
    printf("unwraps to: " BURROW_STR_FMT "\n",
           BURROW_STR_ARG(error_text(errors_unwrap(err))));
}

static void join(Alloc *a) {
    Error close_err = errors_new(a, BURROW_S("close failed"));
    Error flush_err = BURROW_NO_ERROR;
    Error err1 = err_not_found;

    // doc: join
    Error err = errors_join_v(a, 2, close_err, flush_err);
    // doc: end
    printf("joined: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));

    // doc: join-slice
    Slice errs = slice_make(a, TYPE_ERROR, 0, 4);
    errs = BURROW_APPEND(Error, a, errs, err1);
    Error all = errors_join(a, errs);
    // doc: end
    errs = BURROW_APPEND(Error, a, errs, close_err);
    all = errors_join(a, errs);
    printf("joined:\n" BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(all)));
    printf("is not found %d\n", errors_is(all, err_not_found));
}

static void oom(Alloc *a) {
    // doc: oom
    Error err = errors_new(a, BURROW_S("record not found"));
    if (errors_is(err, burrow_err_out_of_memory)) {
        printf("could not even build the error\n");
    }
    // doc: end
    (void)err;
}

static Error handle(Int i) {
    if (i % 1000 == 999)
        return errors_new(error_allocator(), BURROW_S("request failed"));
    return BURROW_NO_ERROR;
}

static void serve(Alloc *a) {
    Error last = BURROW_NO_ERROR;
    // doc: scope
    for (Int i = 0; i < 100000; i++) {
        ArenaMark m = error_mark();
        Error err = handle(i);
        if (BURROW_FAILED(err))
            last = error_retain(a, err); /* kept past the release */
        error_release(m);
    }
    // doc: end
    printf("last: " BURROW_STR_FMT ", error arena holds %llu bytes\n",
           BURROW_STR_ARG(error_text(last)),
           (unsigned long long)mem_stats(error_allocator()).bytes_live);
}

int main(void) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    check();
    make(a);
    read_all();
    extract();
    join(a);
    oom(a);
    serve(a);
    arena_free(&ar);
    return 0;
}

/* Output:
nope: cannot open the file
saved
message of no error is 0 bytes
errors_is the sentinel with the same text: 0
read 3, then EOF
failed on line 12
unwraps to: unexpected comma
joined: close failed
joined:
record not found
close failed
is not found 1
last: request failed, error arena holds 0 bytes
*/

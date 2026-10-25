#define BURROW_SHORT 1
#include <stdio.h>

#include "burrow/burrow.h"

static Chan *done;

static void nap(void *env) {
    (void)env;
    time_sleep(10 * TIME_MILLISECOND);
    Int one = 1;
    chan_send(done, &one);
}

/* BURROW_TRY gets a function to itself, with no compound literals in it. */
static void peek(void) {
    Int xs[] = {1, 2, 3};
    Slice s = slice_from(xs, 3, 3, TYPE_INT);
    Int i = 5;
    // doc: bounds
    BURROW_TRY {
        Int v = AT(Int, s, i);
        printf("%d\n", (int)v);
    }
    BURROW_CATCH(p) {
        Str text = panic_text(p);
        printf("caught: " STR_FMT "\n", STR_ARG(text));
    }
    BURROW_TRY_END;
    // doc: end
}

static void run(void *env) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: str
    Str line = S("name=gopher");
    Str value;
    Str key = strings_cut(line, S("="), &value, NULL);
    printf(STR_FMT " is " STR_FMT ", %d bytes\n", STR_ARG(key), STR_ARG(value),
           (int)value.len);
    char *c = str_to_cstr(a, value);
    puts(c);
    // doc: end

    // doc: errors
    Error err = NO_ERROR;
    OsFile *f = os_open(a, S("no/such/file"), &err);
    if (FAILED(err)) {
        Error wrapped = fmt_errorf_v("loading the config: %w", err);
        bool missing = errors_is(wrapped, os_err_not_exist);
        fmt_println_v(wrapped);
        fmt_println_v(missing);
    } else {
        os_file_close(f);
    }
    // doc: end

    peek();

    // doc: goroutines
    done = chan_make(a, TYPE_INT, 0);
    for (int i = 0; i < 10000; i++)
        go(FN(Func, nap, NULL));
    Int total = 0, one;
    for (int i = 0; i < 10000; i++) {
        chan_recv(done, &one);
        total += one;
    }
    fmt_println_v(total, "goroutines slept");
    // doc: end

    chan_free(done);
    arena_free(&ar);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}

/* Output:
name is gopher, 6 bytes
gopher
loading the config: open no/such/file: no such file or directory
true
caught: runtime error: index out of range [5] with length 3
10000 goroutines slept
*/

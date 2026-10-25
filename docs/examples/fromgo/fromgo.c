#define BURROW_SHORT 1
#include <stdio.h>

#include "burrow/burrow.h"

// doc: struct
#define ACCOUNT_FIELDS(F, T)                                                           \
    F(T, Str, Name, "json:\"name\"")                                                   \
    F(T, Int, Age, "json:\"age,omitempty\"")
BURROW_STRUCT(Account, ACCOUNT_FIELDS);
// doc: end

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: results
    Error err = NO_ERROR;
    Int n = strconv_atoi(S("12a"), &err);
    if (FAILED(err))
        fmt_println_v(err);
    n = strconv_atoi(S("42"), NULL);
    fmt_println_v(n);
    // doc: end

    // doc: cut
    Str after;
    bool found;
    Str before = strings_cut(S("user=gopher"), S("="), &after, &found);
    fmt_println_v(before, after, found);
    // doc: end

    // doc: generic
    Int xs[] = {3, 1, 2};
    Slice s = slice_from(xs, 3, 3, TYPE_INT);
    slices_sort(s);
    Int two = 2;
    fmt_println_v(s, slices_index(s, &two));
    // doc: end

    /* fmt prints through stdio and os_stdout does not, so flush first to keep
     * the lines in order when stdout is a pipe. */
    fflush(stdout);
    // doc: iface
    StringsReader *r = strings_new_reader(a, S("hello, reader\n"));
    io_copy(a, os_file_as_io_writer(os_stdout), strings_reader_as_io_reader(r), &err);
    // doc: end

    // doc: json
    Account u = {S("gopher"), 0};
    Slice out = json_marshal(a, ANY(TYPE_OF(Account), &u), &err);
    fmt_println_v(str_from_bytes(out.p, out.len));
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
strconv.Atoi: parsing "12a": invalid syntax
42
user gopher true
[1 2 3] 1
hello, reader
{"name":"gopher"}
*/

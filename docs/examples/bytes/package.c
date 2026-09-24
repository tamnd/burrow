#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/mem/arena.h"

static void print(const char *label, Slice s) {
    printf("%s: %.*s\n", label, (int)s.len, (const char *)s.p);
}

static void search(void) {
    // doc: search
    Slice line = bytes_trim_space(BURROW_B("  key = value  "));
    Int eq = bytes_index_byte(line, '=');
    Slice key = bytes_trim_space(slice_sub(line, 0, eq));
    Slice value = bytes_trim_space(slice_sub(line, eq + 1, line.len));
    // doc: end
    print("key", key);
    print("value", value);
}

static void split(Alloc *a) {
    // doc: split
    Slice fields = bytes_split(a, BURROW_B("a,b,c"), BURROW_B(","));
    Slice first = BURROW_AT(Slice, fields, 0);
    first = slice_append(a, first, "!", 1);
    Slice second = BURROW_AT(Slice, fields, 1);
    // doc: end
    print("first", first);
    print("second", second);
}

static void change(Alloc *a) {
    // doc: change
    Slice in = BURROW_B("Hello, Gophers");
    Slice upper = bytes_to_upper(a, in);
    Slice swapped = bytes_replace_all(a, upper, BURROW_B("GOPHERS"), BURROW_B("WORLD"));
    bool same = bytes_equal_fold(swapped, BURROW_B("hello, world"));
    // doc: end
    print("swapped", swapped);
    printf("fold: %d\n", same);
}

static void buffer(Alloc *a) {
    // doc: buffer
    BytesBuffer b = BYTES_BUFFER(a);
    bytes_buffer_write_string(&b, BURROW_S("id,name\n"), NULL);
    for (int i = 1; i <= 3; i++)
        fmt_fprintf_v(bytes_buffer_as_io_writer(&b), "%d,user%d\n", i, i);
    Str header = bytes_buffer_read_string(&b, a, '\n', NULL);
    Int left = bytes_buffer_len(&b);
    // doc: end
    printf("header: " BURROW_STR_FMT, BURROW_STR_ARG(header));
    printf("left: %lld\n", (long long)left);

    // doc: drain
    Error err = BURROW_NO_ERROR;
    for (;;) {
        Slice row = bytes_buffer_read_bytes(&b, a, '\n', &err);
        if (BURROW_FAILED(err))
            break;
        printf("row: %.*s", (int)row.len, (const char *)row.p);
    }
    bytes_buffer_free(&b);
    // doc: end
    printf("eof: %d\n", errors_is(err, io_eof));
}

static void reader(Alloc *a) {
    // doc: reader
    BytesReader *r = bytes_new_reader(a, BURROW_B("0123456789"));
    bytes_reader_seek(r, -3, BURROW_IO_SEEK_END, NULL);
    Byte tail[8];
    Int n = bytes_reader_read(r, slice_from(tail, 8, 8, TYPE_BYTE), NULL);
    // doc: end
    printf("tail: %.*s\n", (int)n, (const char *)tail);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    search();
    split(a);
    change(a);
    buffer(a);
    reader(a);
    arena_free(&ar);
    return 0;
}

/* Output:
key: key
value: value
first: a!
second: b
swapped: HELLO, WORLD
fold: 1
header: id,name
left: 24
row: 1,user1
row: 2,user2
row: 3,user3
eof: 1
tail: 789
*/

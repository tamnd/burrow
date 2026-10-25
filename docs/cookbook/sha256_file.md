# Hash a file with SHA-256

Stream a file through a hash without reading it all into memory. A `Hash` has an `IoWriter`, so `io_copy` can feed it straight from the file, and `hex_encode_to_string` turns the sum into the text `sha256sum` prints. The second call shows the error you get for a file that isn't there.

<!-- example: ../examples/cookbook/sha256_file.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/hex.h"

/* The SHA-256 of a file, read in pieces so a large file needs no more
 * memory than a small one. */
static Str file_sum(Alloc *a, Str name, Error *err) {
    OsFile *f = os_open(a, name, err);
    if (f == NULL)
        return (Str){0};
    Hash h = sha256_new(a);
    io_copy(a, hash_as_io_writer(h), os_file_as_io_reader(f), err);
    os_file_close(f);
    if (FAILED(*err))
        return (Str){0};
    return hex_encode_to_string(a, hash_sum(a, h, slice_nil(TYPE_BYTE)));
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = os_write_file(S("hello.txt"), BURROW_B("hello, world\n"), 0644);
    Str sum = file_sum(a, S("hello.txt"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    fmt_printf_v("%s  hello.txt\n", sum);

    file_sum(a, S("missing.txt"), &err);
    fmt_println_v(err);
    arena_free(&ar);
}
```

It prints:

```text
853ff93762a06ddbf722c4ebe9ddd66d8f63ddaea97f521c3ecc20da7c976020  hello.txt
open missing.txt: no such file or directory
```

The program is [docs/examples/cookbook/sha256_file.c](../examples/cookbook/sha256_file.c). CI builds and runs it and checks that it prints this.

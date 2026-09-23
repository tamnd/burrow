#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

BURROW_SENTINEL_ERROR(err_bad_request, "bad request");

// doc: lifetimes
BURROW_OWNS(ret) Str str_clone(Alloc *a, Str s);
BURROW_BORROWS(ret, p) Str str_from_bytes(const void *p, Int n);
// doc: end

static void making(char **argv) {
    // doc: literal
    Str name = BURROW_S("burrow");
    // doc: end
    printf("%lld bytes\n", (long long)name.len);

    Str method = BURROW_S("GET");
    // doc: condition
    if (str_eq(method, BURROW_S("GET"))) {
        printf("a read\n");
    }
    // doc: end

    {
        // doc: cstr
        Str s = str_from_cstr(argv[1]);
        // doc: end
        printf("no argument gives %lld bytes\n", (long long)s.len);
    }
    {
        // doc: bytes
        Byte header[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        Str s = str_from_bytes(header, 4);
        // doc: end
        printf("header ends in %X\n", s.p[3]);
    }

    Str path = BURROW_S("/api/users");
    // doc: print
    printf("path is " BURROW_STR_FMT "\n", BURROW_STR_ARG(path));
    // doc: end
}

static Error check(Str s) {
    // doc: has-nul
    if (str_has_nul(s))
        return err_bad_request;
    // doc: end
    return BURROW_NO_ERROR;
}

static void to_c(Alloc *a) {
    Str name = BURROW_S("config.toml");
    // doc: to-cstr
    char *path = str_to_cstr(a, name);
    // doc: end
    printf("%s is %zu bytes\n", path, strlen(path));

    // doc: truncate
    Str s = BURROW_S("safe\0/../../etc/passwd");
    char *c = str_to_cstr(a, s); /* c is "safe" */
    // doc: end
    printf("%lld bytes in, %zu out\n", (long long)s.len, strlen(c));

    Error err = check(s);
    printf("check says %s\n", BURROW_OK(err) ? "ok" : "bad request");
}

static void compare(void) {
    Str a = BURROW_S("apple"), b = BURROW_S("banana");
    // doc: compare
    bool same = str_eq(a, b);
    int order = str_cmp(a, b);
    // doc: end
    printf("same %d, apple first %d\n", same, order < 0);

    Str zero = {0};
    printf("zeroed and literal empty are equal %d\n", str_eq(zero, BURROW_S("")));
}

static void indexing(void) {
    Str s = BURROW_S("burrow");
    Int total = 0;

    // doc: at
    Byte b = str_at(s, 3);
    // doc: end

    // doc: walk
    for (Int i = 0; i < s.len; i++)
        total += s.p[i];
    // doc: end
    printf("%c, byte sum %lld\n", b, (long long)total);
}

static void lifetimes(Alloc *a) {
    char line[] = "name=burrow";
    Str borrowed = str_from_bytes(line + 5, 6);

    // doc: clone
    Str kept = str_clone(a, borrowed);
    // doc: end
    memset(line, 'x', sizeof line - 1);
    printf("borrowed " BURROW_STR_FMT ", kept " BURROW_STR_FMT "\n",
           BURROW_STR_ARG(borrowed), BURROW_STR_ARG(kept));
}

int main(int argc, char **argv) {
    Arena ar;
    (void)argc;
    arena_init(&ar, heap_allocator(), 0);

    making(argv);
    to_c(arena_allocator(&ar));
    compare();
    indexing();
    lifetimes(arena_allocator(&ar));
    arena_free(&ar);
    return 0;
}

/* Output:
6 bytes
a read
no argument gives 0 bytes
header ends in EF
path is /api/users
config.toml is 11 bytes
22 bytes in, 4 out
check says bad request
same 0, apple first 1
zeroed and literal empty are equal 1
r, byte sum 673
borrowed xxxxxx, kept burrow
*/

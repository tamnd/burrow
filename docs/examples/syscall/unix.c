#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/syscall.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: env
    Error err = syscall_setenv(BURROW_S("GREETING"), BURROW_S("hello"));
    bool found = false;
    Str v = syscall_getenv(a, BURROW_S("GREETING"), &found);
    if (found)
        printf("GREETING=%.*s\n", (int)v.len, (const char *)v.p);
    // doc: end
    if (BURROW_FAILED(err) || !found)
        return 1;

#if !defined(BURROW_OS_WINDOWS) && !defined(BURROW_OS_WASI) && !defined(BURROW_OS_COSMO)
    // doc: mmap
    Slice b = syscall_mmap(-1, 0, syscall_getpagesize(),
                           SYSCALL_PROT_READ | SYSCALL_PROT_WRITE,
                           SYSCALL_MAP_ANON | SYSCALL_MAP_PRIVATE, &err);
    if (BURROW_OK(err)) {
        ((Byte *)b.p)[0] = 1;
        err = syscall_munmap(b);
    }
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
#endif
#if defined(BURROW_OS_LINUX)
    // doc: dirent
    Int fd =
        syscall_open(BURROW_S("/"), SYSCALL_O_RDONLY | SYSCALL_O_DIRECTORY, 0, &err);
    Byte buf[4096];
    Slice names = slice_nil(TYPE_STRING);
    for (;;) {
        Int n = syscall_read_dirent(fd, (Slice){buf, 4096, 4096, TYPE_BYTE}, &err);
        if (n <= 0)
            break;
        syscall_parse_dirent(a, (Slice){buf, n, n, TYPE_BYTE}, -1, names, NULL, &names);
    }
    (void)syscall_close(fd);
    printf("%d entries in /\n", (int)names.len);
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
#endif
    arena_free(&ar);
    return 0;
}

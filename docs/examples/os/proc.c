#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* The child is this program again, told so by its environment. */
    if (os_getenv(a, BURROW_S("EXAMPLE_CHILD")).len > 0)
        os_exit(3);

    // doc: start
    Error err = BURROW_NO_ERROR;
    Str exe = os_executable(a, &err); /* the path of this program */
    Str argv[] = {exe};
    Str env[] = {BURROW_S("EXAMPLE_CHILD=1")};
    OsProcAttr attr = {
        .env = slice_from(env, 1, 1, NULL),
        .files = {0}, /* nothing open in the child */
    };
    OsProcess *p = os_start_process(a, exe, slice_from(argv, 1, 1, NULL), &attr, &err);
    if (BURROW_FAILED(err))
        return 1;
    OsProcessState *ps = os_process_wait(p, &err);
    Str how = os_process_state_string(ps, a);  /* "exit status 3" */
    Int code = os_process_state_exit_code(ps); /* 3 */
    bool ok = os_process_state_success(ps);    /* false */
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
    printf(BURROW_STR_FMT " %d %s\n", BURROW_STR_ARG(how), (int)code,
           ok ? "true" : "false");
    os_process_state_free(ps);
    os_process_free(p);

    // doc: kill
    OsProcess *self = os_find_process(a, os_getpid(), &err);
    Error rerr = os_process_release(self); /* self no longer refers to the process */
    Error kerr = os_process_kill(self);    /* "os: process already released" on Unix */
    // doc: end
    if (BURROW_FAILED(err) || BURROW_FAILED(rerr) || !BURROW_FAILED(kerr))
        return 1;
    os_process_free(self);

    arena_free(&ar);
    return 0;
}

/* Output:
exit status 3 3 false
*/

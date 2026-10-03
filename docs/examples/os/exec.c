#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/context.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/os/exec.h"
#include "burrow/proc.h"
#include "burrow/strings.h"
#include "burrow/time.h"

/* The programs run here are this one again, told what to do by its first
 * argument, so the example works the same everywhere. */
static int child(Alloc *a, Str what) {
    Error err = BURROW_NO_ERROR;
    if (str_eq(what, BURROW_S("greet"))) {
        os_file_write_string(os_stdout, BURROW_S("hello from the child\n"), &err);
        return 0;
    }
    if (str_eq(what, BURROW_S("upper"))) {
        Slice in = io_read_all(a, os_file_as_io_reader(os_stdin), &err);
        Str s = strings_to_upper(a, str_from_bytes(in.p, in.len));
        os_file_write_string(os_stdout, s, &err);
        return 0;
    }
    if (str_eq(what, BURROW_S("fail"))) {
        os_file_write_string(os_stderr, BURROW_S("something went wrong\n"), &err);
        return 3;
    }
    if (str_eq(what, BURROW_S("sleep")))
        time_sleep(10 * TIME_SECOND);
    return 0;
}

/* Says what went wrong, the way log.Fatal would. */
static int fail(Error err) {
    fprintf(stderr, BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
    return 1;
}

/* A context's timer needs the runtime, so this part runs under runtime_main. */
static void with_timeout(void *env) {
    Str exe = *(const Str *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: context
    ContextCancelFunc cancel;
    Context ctx =
        context_with_timeout(a, context_background(), 100 * TIME_MILLISECOND, &cancel);
    ExecCmd *c = exec_command_context_v(a, ctx, exe, 1, BURROW_S("sleep"));
    Error err = exec_cmd_run(c); /* killed after a tenth of a second */
    if (BURROW_FAILED(err))
        printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(context_err(ctx))));
    exec_cmd_free(c);
    BURROW_CALLF0(cancel);
    context_release(ctx);
    // doc: end

    arena_free(&ar);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Slice args = os_args();
    if (args.len > 1)
        return child(a, BURROW_AT(Str, args, 1));
    Error err = BURROW_NO_ERROR;
    Str exe = os_executable(a, &err);
    if (BURROW_FAILED(err))
        return fail(err);

    // doc: output
    ExecCmd *c = exec_command_v(a, exe, 1, BURROW_S("greet"));
    Slice out = exec_cmd_output(c, a, &err);
    if (BURROW_FAILED(err))
        return fail(err);
    printf("%.*s", (int)out.len, (const char *)out.p); /* hello from the child */
    exec_cmd_free(c);
    // doc: end

    // doc: stdin
    StringsReader in;
    strings_reader_reset(&in, BURROW_S("some input\n"));
    c = exec_command_v(a, exe, 1, BURROW_S("upper"));
    c->stdin_ = strings_reader_as_io_reader(&in); /* copied to the child on a pipe */
    out = exec_cmd_output(c, a, &err);
    if (BURROW_FAILED(err))
        return fail(err);
    printf("%.*s", (int)out.len, (const char *)out.p); /* SOME INPUT */
    exec_cmd_free(c);
    // doc: end

    // doc: exit
    c = exec_command_v(a, exe, 1, BURROW_S("fail"));
    (void)exec_cmd_output(c, a, &err);
    const ExecExitError *ee = errors_as(err, TYPE_EXEC_EXIT_ERROR);
    if (ee != NULL) {
        printf(BURROW_STR_FMT "\n",
               BURROW_STR_ARG(error_text(err))); /* exit status 3 */
        printf("code %d, stderr %.*s",
               (int)os_process_state_exit_code(ee->process_state), (int)ee->stderr_.len,
               (const char *)ee->stderr_.p);
    }
    exec_cmd_free(c);
    // doc: end

    runtime_main(BURROW_FN(Func, with_timeout, &exe));

    // doc: lookpath
    Str path = exec_look_path(a, BURROW_S("no-such-program"), &err);
    if (errors_is(err, exec_err_not_found))
        printf("not found, path %d bytes\n", (int)path.len);
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
hello from the child
SOME INPUT
exit status 3
code 3, stderr something went wrong
context deadline exceeded
not found, path 0 bytes
*/

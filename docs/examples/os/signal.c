#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/context.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/os/signal.h"
#include "burrow/syscall.h"

/* The signals here are sent by the program to itself, so it runs without
 * anyone pressing Ctrl-C. The docs examples run on Linux and macOS only, and
 * Windows has no way for a process to send itself Ctrl-C. */
static void interrupt_self(Alloc *a) {
    Error err = BURROW_NO_ERROR;
    OsProcess *p = os_find_process(a, os_getpid(), &err);
    if (BURROW_OK(err))
        (void)os_process_signal(p, os_interrupt);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: notify
    Chan *c = chan_make(a, TYPE_OS_SIGNAL, 1);
    signal_notify_v(c, 2, os_interrupt, os_signal_from_syscall(SYSCALL_SIGTERM));
    interrupt_self(a); /* or Ctrl-C */
    OsSignal s;
    chan_recv(c, &s);
    Str name = s.vt->string(s.data, a);
    printf("got " BURROW_STR_FMT "\n", BURROW_STR_ARG(name)); /* got interrupt */
    signal_stop(c);
    // doc: end

    // doc: context
    ContextCancelFunc stop;
    Context ctx =
        signal_notify_context_v(a, context_background(), &stop, 1, os_interrupt);
    interrupt_self(a);
    chan_recv(context_done(ctx), NULL);
    Str why = error_text(context_cause(ctx));
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(why)); /* interrupt signal received */
    BURROW_CALLF0(stop);
    context_release(ctx);
    // doc: end

    // doc: ignore
    signal_ignore_v(1, os_signal_from_syscall(SYSCALL_SIGHUP));
    bool ignored = signal_ignored(os_signal_from_syscall(SYSCALL_SIGHUP)); /* true */
    signal_reset_v(0); /* SIGHUP stays ignored, as in Go */
    // doc: end
    printf("ignored %d\n", (int)ignored);

    arena_free(&ar);
    return 0;
}

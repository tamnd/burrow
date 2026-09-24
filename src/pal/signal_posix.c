/* Handlers and signal stacks on everything with sigaction.
 *
 * This was the second half of src/runtime/stack.c until the platform layer
 * existed, and a good deal of the reasoning in the comments came with it. What
 * is new is that it handles any signal rather than the two a guard page raises,
 * and that the caller above says whether a fault was its own by returning a
 * bool rather than by knowing what a struct sigaction is.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* Before every include, because a feature macro only counts if nothing has been
 * included yet. C11 on its own gives signal() and raise() and nothing else, so
 * sigaction, siginfo_t, sigaltstack and SA_ONSTACK are all invisible to a strict
 * build without this, on glibc and on musl alike. _WIN32 rather than
 * BURROW_OS_WINDOWS because knowing the latter would mean including a header
 * first. */
#if !defined(_WIN32)
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* How many native signal numbers this file will keep a row for.
 *
 * NSIG is not in POSIX and the platforms that do define it disagree: 32 on
 * macOS, 65 on glibc with the real time signals, 33 on musl unless you ask for
 * the extensions. A number of our own that is at least as large as any of them
 * costs a few kilobytes of BSS and means a table lookup never has to trust a
 * macro that might not be there. Anything outside it is refused by name below
 * rather than quietly indexed. */
#define SIG_SLOTS 65

/* Somewhere to put a function pointer that an atomic will take.
 *
 * The atomics in burrow/atomic.h deal in void * and ISO C does not let a
 * function pointer be cast to one: the two are the same width on every machine
 * anybody has built this for, and the standard still declines to say so.
 * POSIX does say so, which is the only reason dlsym has the return type it
 * has, and this file is the POSIX backend. Going through a union rather than a
 * cast is how that is said without a pedantic build arguing about it, and the
 * assertion underneath is what fails the build on a machine where it stops
 * being true. */
typedef union Handler {
    void *object;
    PalSignalHandler fn;
} Handler;

_Static_assert(sizeof(PalSignalHandler) == sizeof(void *),
               "a function pointer does not fit where this file puts one");

/* One row per native signal, and all three are written by the installer and
 * read by the handler.
 *
 * The handler is published with a release store so that a signal arriving on
 * another thread the moment sigaction returns finds either a whole handler or
 * none. The other two rows are only read once that pointer has been seen, which
 * is what makes reading them plain. */
static Handler handlers[SIG_SLOTS];
static int32_t pal_numbers[SIG_SLOTS];
static struct sigaction previous[SIG_SLOTS];

/* SIGSEGV and SIGBUS are the two the table has to have room for, since
 * PAL_SIGFAULT is installed for both by number rather than by lookup.
 *
 * Cosmopolitan is the exception. One of its binaries runs on systems that
 * number their signals differently, so its signal macros are variables that
 * are filled in at startup, and there is nothing here for the compiler to
 * check. install_one checks the same thing at run time on every platform, and
 * every system Cosmopolitan runs on keeps these two below 32. */
#if !defined(BURROW_OS_COSMO)
_Static_assert(SIGSEGV < SIG_SLOTS && SIGBUS < SIG_SLOTS,
               "the signal table is too small for the fault signals");
#endif

static PalSignalHandler handler_of(int native) {
    Handler h;

    h.object = burrow__atomic_load_acquire_ptr(&handlers[native].object);
    return h.fn;
}

static void set_handler(int native, PalSignalHandler fn) {
    Handler h;

    h.object = NULL;
    h.fn = fn;
    burrow__atomic_store_release_ptr(&handlers[native].object, h.object);
}

/* The signal stack this thread is on, kept so that removing it can give the
 * mapping back. Thread local, and only the thread it belongs to ever looks. */
static BURROW_THREAD_LOCAL void *altstack;
static BURROW_THREAD_LOCAL int64_t altstack_bytes;

/* Not const under Cosmopolitan, for the reason above: the right hand column is
 * only known at startup, and its compiler fills the table in then and warns
 * about doing it to a const one. */
#if defined(BURROW_OS_COSMO)
#define SIGNAL_PAIRS_CONST
#else
#define SIGNAL_PAIRS_CONST const
#endif

/* Ours on the left, the platform's on the right.
 *
 * PAL_SIGFAULT is not in here on purpose. It is not one signal and the install
 * below takes it apart rather than looking it up.
 *
 * A table rather than a switch, because nineteen cases that each return a
 * different constant are nineteen branches a clone detector reads as copies of
 * one another, and because two columns is how a mapping wants to be read. */
static SIGNAL_PAIRS_CONST struct {
    int32_t pal;
    int native;
} signal_pairs[] = {
    {PAL_SIGHUP, SIGHUP},
    {PAL_SIGINT, SIGINT},
    {PAL_SIGQUIT, SIGQUIT},
    {PAL_SIGILL, SIGILL},
    {PAL_SIGTRAP, SIGTRAP},
    {PAL_SIGABRT, SIGABRT},
    {PAL_SIGBUS, SIGBUS},
    {PAL_SIGFPE, SIGFPE},
    {PAL_SIGKILL, SIGKILL},
    {PAL_SIGUSR1, SIGUSR1},
    {PAL_SIGSEGV, SIGSEGV},
    {PAL_SIGUSR2, SIGUSR2},
    {PAL_SIGPIPE, SIGPIPE},
    {PAL_SIGALRM, SIGALRM},
    {PAL_SIGTERM, SIGTERM},
    {PAL_SIGCHLD, SIGCHLD},
    {PAL_SIGCONT, SIGCONT},
    {PAL_SIGSTOP, SIGSTOP},
    /* SIGURG is what Go uses for this and the reasons are the same ones: it is
     * a signal nothing else in a normal program raises, the default disposition
     * is to ignore it, and it does not interrupt a system call in a way that
     * loses data. */
    {PAL_SIGPREEMPT, SIGURG},
};

/* One of ours to one of the platform's, or -1. */
static int to_native(int32_t sig) {
    for (size_t i = 0; i < sizeof signal_pairs / sizeof signal_pairs[0]; i++) {
        if (signal_pairs[i].pal == sig)
            return signal_pairs[i].native;
    }

    return -1;
}

int32_t burrow__pal_signal_from_native(int native) {
    for (size_t i = 0; i < sizeof signal_pairs / sizeof signal_pairs[0]; i++) {
        if (signal_pairs[i].native == native)
            return signal_pairs[i].pal;
    }
    return (int32_t)native;
}

bool pal_kill(int64_t pid, int32_t sig, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    int native = sig == 0 ? 0 : to_native(sig);
    if (native < 0 || pid <= 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    if (kill((pid_t)pid, native) != 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }
    return true;
}

/* Hands the signal back to whoever had it and lets it happen again.
 *
 * A handler that returns from a fault it did not cause would fault again at the
 * same instruction forever, so the only way out of "this one is not mine" is to
 * put the old disposition back first. If that was SIG_DFL the program then dies
 * the way it would have, core file and all, and if it was another handler that
 * handler sees it. */
static void forward(int native, siginfo_t *info, void *uc) {
    const struct sigaction *prev = &previous[native];

    /* Both sides cast, because sa_flags is a signed int and so is SA_SIGINFO on
     * every libc here, and masking one signed thing with another is the kind of
     * thing a linter is right to ask about even when the bit in question is a
     * small positive constant. The types are the libc's to choose, so the cast
     * is the only end of it we own. */
    if (((unsigned int)prev->sa_flags & (unsigned int)SA_SIGINFO) != 0U &&
        prev->sa_sigaction != NULL) {
        prev->sa_sigaction(native, info, uc);
        return;
    }

    if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN &&
        prev->sa_handler != NULL) {
        prev->sa_handler(native);
        return;
    }

    (void)sigaction(native, prev, NULL);
}

static void dispatch(int native, siginfo_t *info, void *uc) {
    if (native < 0 || native >= SIG_SLOTS)
        return;

    PalSignalHandler handler = handler_of(native);
    if (handler != NULL && handler(pal_numbers[native], info, uc))
        return;

    forward(native, info, uc);
}

/* Takes one native signal. The caller below is what turns one PAL number into
 * one or two of these. */
static bool install_one(int native, int32_t sig, PalSignalHandler handler,
                        PalErrno *err) {
    if (native < 0 || native >= SIG_SLOTS) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    /* The handler pointer goes in first and with a release, so that a signal
     * arriving on another thread the instant sigaction returns finds a handler
     * rather than a NULL. The row is useless until sigaction has run, so
     * writing it early costs nothing. */
    bool first = handler_of(native) == NULL;

    pal_numbers[native] = sig;
    set_handler(native, handler);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = dispatch;
    /* SA_ONSTACK is what sends the handler to the stack pal_signal_stack_install
     * gave the thread, which is the only stack left when the ordinary one has
     * overflowed. SA_SIGINFO is what makes the faulting address available at
     * all. SA_RESTART so that a signal arriving during a read does not turn
     * into an EINTR the caller above never asked to handle. */
    sa.sa_flags = SA_ONSTACK | SA_SIGINFO | SA_RESTART;
    (void)sigemptyset(&sa.sa_mask);

    /* Only the first install keeps what was there, because the second one would
     * otherwise record our own dispatcher as the thing to forward to, and
     * forwarding to yourself is a loop with a signal in it. */
    struct sigaction was;
    memset(&was, 0, sizeof was);
    if (sigaction(native, &sa, &was) != 0) {
        set_handler(native, NULL);
        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }
    if (first)
        previous[native] = was;

    BURROW_OUT(err, PAL_OK);
    return true;
}

bool pal_signal_install(int32_t sig, PalSignalHandler handler, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (handler == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    /* The one that is not a signal. SIGSEGV and SIGBUS both, because Linux
     * raises the first for a guard page and macOS raises the second for some of
     * the same accesses, and a caller that had to know which is a caller this
     * layer had failed.
     *
     * Either failing undoes the other, so a caller that gets false is not left
     * with half of a handler installed. */
    if (sig == PAL_SIGFAULT) {
        if (!install_one(SIGSEGV, sig, handler, err))
            return false;
        if (!install_one(SIGBUS, sig, handler, err)) {
            (void)sigaction(SIGSEGV, &previous[SIGSEGV], NULL);
            set_handler(SIGSEGV, NULL);
            return false;
        }
        return true;
    }

    int native = to_native(sig);
    if (native < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    /* SIGKILL and SIGSTOP cannot be caught and the system says so with EINVAL.
     * It is said here as well, so that the answer is the same on a platform
     * whose libc decides to be helpful about it. */
    if (native == SIGKILL || native == SIGSTOP) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    /* A bad memory access is PAL_SIGFAULT's, and a second handler on one of its
     * two signals would quietly take half of it away. */
    if (native == SIGSEGV || native == SIGBUS) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    return install_one(native, sig, handler, err);
}

bool pal_signal_mask(int32_t sig, bool block, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    int native = to_native(sig);
    if (native < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    sigset_t set;
    if (sigemptyset(&set) != 0 || sigaddset(&set, native) != 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }

    /* pthread_sigmask and not sigprocmask. The second one is undefined in a
     * process with more than one thread, and every process burrow runs in has
     * more than one thread the moment the scheduler starts. */
    int rc = pthread_sigmask(block ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL);
    if (rc != 0) {
        BURROW_OUT(err, burrow__pal_errno(rc));
        return false;
    }

    return true;
}

/* How big a signal stack has to be.
 *
 * MINSIGSTKSZ is 5120 on glibc, 6144 on musl and 32768 on macOS arm64, and on
 * glibc since 2.34 it is not a constant at all but a call into the dynamic
 * loader, because a machine with wider vector registers needs a bigger signal
 * frame. So the number is asked for at runtime and a floor is applied under it,
 * and the floor is 64 kilobytes because the handler above this layer formats a
 * message and writes it, which MINSIGSTKSZ does not account for. */
static int64_t stack_bytes(void) {
    int64_t want = INT64_C(64) * 1024;

#if defined(_SC_SIGSTKSZ)
    long answer = sysconf(_SC_SIGSTKSZ);
    if (answer > 0 && (int64_t)answer > want)
        want = (int64_t)answer;
#endif

    if ((int64_t)MINSIGSTKSZ > want)
        want = (int64_t)MINSIGSTKSZ;

    int64_t page = pal_page_size();
    if (page <= 0)
        return want;

    int64_t over = want % page;
    if (over != 0)
        want += page - over;
    return want;
}

bool pal_signal_stack_install(PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (altstack != NULL)
        return true;

    int64_t bytes = stack_bytes();

    /* Reserve and then commit, rather than one mapping that is readable from
     * the start, because that is the shape this layer offers and a signal stack
     * is not special enough to reach past it. Two system calls once per thread
     * is not a price worth naming. */
    void *base = pal_vm_reserve(bytes, err);
    if (base == NULL)
        return false;
    if (!pal_vm_commit(base, bytes, err)) {
        (void)pal_vm_release(base, bytes, NULL);
        return false;
    }

    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = base;
    ss.ss_size = (size_t)bytes;
    ss.ss_flags = 0;

    if (sigaltstack(&ss, NULL) != 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        (void)pal_vm_release(base, bytes, NULL);
        return false;
    }

    altstack = base;
    altstack_bytes = bytes;
    return true;
}

void pal_signal_stack_remove(void) {
    if (altstack == NULL)
        return;

    stack_t off;
    memset(&off, 0, sizeof off);
    off.ss_flags = SS_DISABLE;
    (void)sigaltstack(&off, NULL);

    (void)pal_vm_release(altstack, altstack_bytes, NULL);

    altstack = NULL;
    altstack_bytes = 0;
}

const void *pal_signal_fault_addr(const void *info) {
    if (info == NULL)
        return NULL;

    /* si_addr is a member of a union on some platforms and a plain member on
     * others, and either way this is the documented way to ask. It only means
     * anything for SIGSEGV, SIGBUS, SIGILL and SIGFPE, which is why the header
     * says to only ask inside a PAL_SIGFAULT handler. */
    const siginfo_t *si = (const siginfo_t *)info;
    return si->si_addr;
}

#endif /* !BURROW_OS_WINDOWS */

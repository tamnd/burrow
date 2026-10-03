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
#include <fcntl.h>
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

/* Whether previous has been filled in for a signal. Both installers below keep
 * what was there before the first of them touched the signal and never again,
 * so that neither of them can record the other's dispatcher as the thing to go
 * back to. Written under install_mu. */
static unsigned char saved[SIG_SLOTS];

/* Serialises the installers, pal_signal_install and pal_signal_relay, which can
 * be called from any thread. The handler never takes it. */
static pthread_mutex_t install_mu = PTHREAD_MUTEX_INITIALIZER;

/* What pal_signal_relay last asked for, read by the handler. RELAY_NONE until
 * the first call. */
enum { RELAY_NONE = 0, RELAY_CATCH = 1, RELAY_IGNORE = 2 };
static uint32_t relay_state[SIG_SLOTS];

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

bool pal_kill_native(int64_t pid, int32_t sig, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (kill((pid_t)pid, (int)sig) != 0) {
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

/* The function pal_signal_relay_init was given, behind the same kind of union
 * as the handlers and for the same reason. */
typedef union Relay {
    void *object;
    PalSignalRelay fn;
} Relay;

static Relay relay_send;

static bool relay(int native) {
    Relay r;

    r.object = burrow__atomic_load_acquire_ptr(&relay_send.object);
    if (r.fn == NULL)
        return false;

    /* The send function wakes a pipe, and a write that fails sets errno in the
     * middle of whatever the interrupted code was doing with it. */
    int e = errno;
    bool took = r.fn((int32_t)native);
    errno = e;
    return took;
}

static void dispatch(int native, siginfo_t *info, void *uc) {
    if (native < 0 || native >= SIG_SLOTS)
        return;

    PalSignalHandler handler = handler_of(native);
    if (handler != NULL && handler(pal_numbers[native], info, uc))
        return;

    uint32_t state = burrow__atomic_load_acquire_u32(&relay_state[native]);
    if (state == RELAY_IGNORE)
        return;
    if (state == RELAY_CATCH && relay(native))
        return;

    forward(native, info, uc);

    /* A signal that is only here because os/signal asked for it once, and that
     * nobody wants now. That is the moment between a Stop and the disposition
     * going back, and Go's answer is that the signal either reaches a channel
     * or does what it would have done without the program. forward has put the
     * old disposition back, so sending the signal again lets it do that: it is
     * blocked while this handler runs and arrives the moment it returns. A
     * fault cannot get here, since relaying one is refused. */
    if (handler == NULL && previous[native].sa_handler == SIG_DFL &&
        (((unsigned int)previous[native].sa_flags & (unsigned int)SA_SIGINFO) == 0U))
        (void)raise(native);
}

/* The sigaction that sends a signal to dispatch, for both installers.
 *
 * SA_ONSTACK is what sends the handler to the stack pal_signal_stack_install
 * gave the thread, which is the only stack left when the ordinary one has
 * overflowed. SA_SIGINFO is what makes the faulting address available at all.
 * SA_RESTART so that a signal arriving during a read does not turn into an
 * EINTR the caller above never asked to handle. */
static void dispatch_action(struct sigaction *sa) {
    memset(sa, 0, sizeof *sa);
    sa->sa_sigaction = dispatch;
    sa->sa_flags = SA_ONSTACK | SA_SIGINFO | SA_RESTART;
    (void)sigemptyset(&sa->sa_mask);
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
    pal_numbers[native] = sig;
    set_handler(native, handler);

    struct sigaction sa;
    dispatch_action(&sa);

    /* Only the first install keeps what was there, because the second one would
     * otherwise record our own dispatcher as the thing to forward to, and
     * forwarding to yourself is a loop with a signal in it. */
    struct sigaction was;
    memset(&was, 0, sizeof was);
    (void)pthread_mutex_lock(&install_mu);
    if (sigaction(native, &sa, &was) != 0) {
        int e = errno;
        (void)pthread_mutex_unlock(&install_mu);
        set_handler(native, NULL);
        BURROW_OUT(err, burrow__pal_errno(e));
        return false;
    }
    if (!saved[native]) {
        previous[native] = was;
        saved[native] = 1;
    }
    (void)pthread_mutex_unlock(&install_mu);

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

/* ---------------------------------------------------------------- relaying */

/* The note between the handler and os/signal's reader: a pipe, because write
 * is safe in a handler and the futex this layer offers is not on the systems
 * where it is a mutex and a condition variable underneath. Go uses a pipe on
 * macOS for the same reason. */
static int note_fds[2] = {-1, -1};

static bool cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool pal_signal_relay_init(PalSignalRelay send, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    (void)pthread_mutex_lock(&install_mu);
    if (note_fds[0] < 0) {
        int fds[2];
        if (pipe(fds) != 0) {
            int e = errno;
            (void)pthread_mutex_unlock(&install_mu);
            BURROW_OUT(err, burrow__pal_errno(e));
            return false;
        }

        /* The write end does not block. A wake that finds the pipe full has
         * nothing to add, and a handler that blocks is a program that stops. */
        int fl = fcntl(fds[1], F_GETFL);
        if (!cloexec(fds[0]) || !cloexec(fds[1]) || fl < 0 ||
            fcntl(fds[1], F_SETFL, fl | O_NONBLOCK) != 0) {
            int e = errno;
            (void)close(fds[0]);
            (void)close(fds[1]);
            (void)pthread_mutex_unlock(&install_mu);
            BURROW_OUT(err, burrow__pal_errno(e));
            return false;
        }
        note_fds[0] = fds[0];
        note_fds[1] = fds[1];
    }

    Relay r;
    r.object = NULL;
    r.fn = send;
    burrow__atomic_store_release_ptr(&relay_send.object, r.object);
    (void)pthread_mutex_unlock(&install_mu);
    return true;
}

/* Go's sigtable says which signals os/signal may have, and this is that table
 * written as the exceptions. The synchronous ones are the program's own faults,
 * SIGPROF belongs to the profiler, and the real time signals below SIGRTMIN on
 * Linux are the ones glibc and musl use to cancel threads and to change ids.
 * Comparisons rather than a switch because Cosmopolitan's signal numbers are
 * variables. */
static bool relayable(int native) {
    if (native <= 0 || native >= SIG_SLOTS)
        return false;
    if (native == SIGKILL || native == SIGSTOP || native == SIGSEGV ||
        native == SIGBUS || native == SIGFPE || native == SIGILL || native == SIGTRAP ||
        native == SIGSYS || native == SIGPROF)
        return false;
#if defined(SIGSTKFLT)
    if (native == SIGSTKFLT)
        return false;
#endif
#if defined(SIGEMT)
    if (native == SIGEMT)
        return false;
#endif
#if defined(SIGTHR)
    if (native == SIGTHR)
        return false;
#endif
#if defined(BURROW_OS_LINUX) && defined(SIGRTMIN)
    if (native >= 32 && native < SIGRTMIN)
        return false;
#endif
    return true;
}

bool pal_signal_relay(int32_t native, int32_t how, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (!relayable((int)native) ||
        (how != PAL_RELAY_CATCH && how != PAL_RELAY_DEFAULT &&
         how != PAL_RELAY_IGNORE)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    int n = (int)native;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);

    (void)pthread_mutex_lock(&install_mu);

    /* Whatever happens next replaces the disposition, so this is the last
     * chance to see the one the program started with. */
    if (!saved[n]) {
        if (sigaction(n, NULL, &previous[n]) != 0) {
            int e = errno;
            (void)pthread_mutex_unlock(&install_mu);
            BURROW_OUT(err, burrow__pal_errno(e));
            return false;
        }
        saved[n] = 1;
    }

    /* With a PAL handler on the signal too, dispatch stays where it is and the
     * state is all that changes, so the handler keeps getting its turn first. */
    bool shared = handler_of(n) != NULL;
    uint32_t was = burrow__atomic_load_u32(&relay_state[n]);
    int rc = 0;

    switch (how) {
    case PAL_RELAY_CATCH:
        /* The state goes first, so that the signal arriving the instant
         * sigaction returns is relayed rather than forwarded. */
        burrow__atomic_store_release_u32(&relay_state[n], RELAY_CATCH);
        dispatch_action(&sa);
        rc = sigaction(n, &sa, NULL);
        break;
    case PAL_RELAY_DEFAULT:
        if (was != RELAY_CATCH)
            break;
        /* The disposition goes first and the state after, so that a signal in
         * between is still taken by dispatch, which forwards it. */
        if (!shared)
            rc = sigaction(n, &previous[n], NULL);
        burrow__atomic_store_release_u32(&relay_state[n], RELAY_NONE);
        break;
    default:
        burrow__atomic_store_release_u32(&relay_state[n], RELAY_IGNORE);
        if (!shared) {
            sa.sa_handler = SIG_IGN;
            (void)sigemptyset(&sa.sa_mask);
            rc = sigaction(n, &sa, NULL);
        }
        break;
    }

    int e = errno;
    (void)pthread_mutex_unlock(&install_mu);
    if (rc != 0) {
        BURROW_OUT(err, burrow__pal_errno(e));
        return false;
    }
    return true;
}

bool pal_signal_ignored(int32_t native) {
    if (native <= 0 || native >= SIG_SLOTS)
        return false;

    struct sigaction cur;
    memset(&cur, 0, sizeof cur);
    if (sigaction((int)native, NULL, &cur) != 0)
        return false;
    return (((unsigned int)cur.sa_flags & (unsigned int)SA_SIGINFO) == 0U) &&
           cur.sa_handler == SIG_IGN;
}

void pal_signal_note_wake(void) {
    int fd = note_fds[1];
    if (fd < 0)
        return;

    int e = errno;
    char b = 0;
    ssize_t n = write(fd, &b, 1);
    (void)n;
    errno = e;
}

void pal_signal_note_sleep(void) {
    int fd = note_fds[0];
    if (fd < 0)
        return;

    for (;;) {
        char b;
        ssize_t n = read(fd, &b, 1);
        if (n == 1 || (n < 0 && errno != EINTR && errno != EAGAIN) || n == 0)
            return;
    }
}

#endif /* !BURROW_OS_WINDOWS */

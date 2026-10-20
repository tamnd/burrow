/* The CPU profiler: a timer that interrupts the process hz times a second of
 * CPU time it uses, a signal handler that writes down where each interruption
 * landed, and the log runtime/pprof reads that out of.
 *
 * The timer is setitimer's ITIMER_PROF, which counts the CPU time of the whole
 * process and sends SIGPROF to whichever thread is running when it runs out.
 * The handler takes the pc and the frame pointer out of the interrupted
 * context, walks the frames from there if they are on the stack of the
 * goroutine the thread is running, and adds the stack to the log with the
 * goroutine's profiling labels. Go also has a timer per thread on Linux, which
 * is more accurate when many threads are busy at once, and a profiling thread
 * on Windows, which has no SIGPROF. burrow has neither yet, so Windows and
 * wasip1 get a profile with no samples in it.
 *
 * Derived from Go's src/runtime/cpuprof.go, src/runtime/proc.go and
 * src/runtime/signal_unix.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/profbuf.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/lock.h"
#include "burrow/mcontext.h"
#include "burrow/pal.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"
#include "burrow/trace.h"

#include <errno.h>
#include <stdio.h>

#define MAX_CPU_PROF_STACK 64
#define PROF_BUF_WORD_COUNT (1 << 17)
#define PROF_BUF_TAG_COUNT (1 << 14)

typedef struct CpuProfile {
    burrow__Lock lock;
    bool on;              /* profiling is on */
    burrow__ProfBuf *log; /* profile events written here */

    /* extra holds extra stacks accumulated in addNonGo corresponding to
     * profiling signals arriving on threads that are not running a goroutine.
     * Those signals arrive when nothing is guarding the log, so they are
     * written here instead and added to the log by the next goroutine that
     * takes a sample, or when the profile stops. */
    uintptr_t extra[1000];
    Int num_extra;
    uint64_t lost_extra; /* count of frames lost because extra is full */

    /* Count of frames lost because of being in the 32 bit atomics' lock table.
     * Go updates its counter racily, and this one is an atomic so that the race
     * is not undefined behaviour as well. */
    uint32_t lost_atomic;
} CpuProfile;

static CpuProfile cpuprof;

/* Go's prof.signalLock and prof.hz. The lock is taken by the signal handler and
 * by whoever changes the rate, and hz is read without it. */
static uint32_t prof_signal_lock;
static uint32_t prof_hz;

/* Whether the SIGPROF handler is in, which it stays once it is. */
static uint32_t prof_handler_installed;

/* The functions a sample is attributed to when there is no stack to give it,
 * Go's _System, _ExternalCode, _LostExternalCode and _LostSIGPROFDuringAtomic64.
 * Only their addresses are used. The names show up in the pprof graphs. Go's
 * call themselves to have a body; these panic, which also gives a linker that
 * folds identical functions together nothing to fold them with. */
BURROW_NOINLINE void burrow__prof_system(void) {
    runtime_throw(BURROW_S("runtime: _System called"));
}

BURROW_NOINLINE void burrow__prof_external_code(void) {
    runtime_throw(BURROW_S("runtime: _ExternalCode called"));
}

BURROW_NOINLINE void burrow__prof_lost_external_code(void) {
    runtime_throw(BURROW_S("runtime: _LostExternalCode called"));
}

BURROW_NOINLINE void burrow__prof_lost_sigprof_during_atomic64(void) {
    runtime_throw(BURROW_S("runtime: _LostSIGPROFDuringAtomic64 called"));
}

/* A marker's address as a return address, one past where it starts, which is
 * what the profile builder takes one back off again. Go's sys.PCQuantum. */
#define MARKER_PC(f) ((uintptr_t)(f) + 1U)

static void signal_lock(void) {
    for (;;) {
        uint32_t idle = 0;
        if (burrow__atomic_cas_u32(&prof_signal_lock, &idle, 1))
            return;
        pal_thread_yield();
    }
}

static void signal_unlock(void) {
    burrow__atomic_store_release_u32(&prof_signal_lock, 0);
}

/* addExtra adds the "extra" profiling events, queued by addNonGo, to the
 * profile log. addExtra is called either from a signal handler on a thread
 * running a goroutine or from runtime_set_cpu_profile_rate. */
static void add_extra(CpuProfile *p) {
    /* Copy accumulated non-Go profile events. */
    const uint64_t hdr[1] = {1};
    for (Int i = 0; i < p->num_extra;) {
        Int n = (Int)p->extra[i];
        burrow__prof_buf_write(p->log, NULL, 0, hdr, 1, &p->extra[i + 1], n - 1);
        i += n;
    }
    p->num_extra = 0;

    /* Report any lost events. */
    if (p->lost_extra > 0) {
        const uint64_t lost_hdr[1] = {p->lost_extra};
        const uintptr_t lost_stk[2] = {
            MARKER_PC(burrow__prof_lost_external_code),
            MARKER_PC(burrow__prof_external_code),
        };
        burrow__prof_buf_write(p->log, NULL, 0, lost_hdr, 1, lost_stk, 2);
        p->lost_extra = 0;
    }

    uint32_t lost_atomic = burrow__atomic_swap_u32(&p->lost_atomic, 0);
    if (lost_atomic > 0) {
        const uint64_t lost_hdr[1] = {lost_atomic};
        const uintptr_t lost_stk[2] = {
            MARKER_PC(burrow__prof_lost_sigprof_during_atomic64),
            MARKER_PC(burrow__prof_system),
        };
        burrow__prof_buf_write(p->log, NULL, 0, lost_hdr, 1, lost_stk, 2);
    }
}

/* add adds the stack trace to the profile. It is called from signal handlers
 * and other limited environments and cannot allocate memory or acquire locks
 * that might be held at the time of the signal, nor can it use substantial
 * amounts of stack. */
static void cpuprof_add(void *const *tag_ptr, const uintptr_t *stk, Int n) {
    /* Simple cas-lock to coordinate with set_cpu_profile_rate. */
    signal_lock();

    if (burrow__atomic_load_relaxed_u32(&prof_hz) !=
        0) { /* implies cpuprof.log != NULL */
        if (cpuprof.num_extra > 0 || cpuprof.lost_extra > 0 ||
            burrow__atomic_load_relaxed_u32(&cpuprof.lost_atomic) > 0)
            add_extra(&cpuprof);
        const uint64_t hdr[1] = {1};
        /* Note: write "knows" that the argument is &curg->labels, and that a
         * NULL there, or no tag pointer at all, is no labels. */
        burrow__prof_buf_write(cpuprof.log, tag_ptr, burrow__nanotime(), hdr, 1, stk,
                               n);
    }

    signal_unlock();
}

/* addNonGo adds the non-Go stack trace to the profile. It is called from a
 * signal on a thread with no M, so it cannot write to the log, which only the
 * threads running goroutines and the rate changes coordinate on. The stack is
 * kept in extra until one of those does. */
static void cpuprof_add_non_go(const uintptr_t *stk, Int n) {
    /* Simple cas-lock to coordinate with set_cpu_profile_rate. */
    signal_lock();

    if (cpuprof.num_extra + 1 + n <
        (Int)(sizeof cpuprof.extra / sizeof cpuprof.extra[0])) {
        Int i = cpuprof.num_extra;
        cpuprof.extra[i] = (uintptr_t)(1 + n);
        for (Int j = 0; j < n; j++)
            cpuprof.extra[i + 1 + j] = stk[j];
        cpuprof.num_extra += 1 + n;
    } else {
        cpuprof.lost_extra++;
    }

    signal_unlock();
}

/* Whether pc is in the middle of switching goroutines, where the stack pointer
 * may already be on the new stack while the frame pointer is still the old
 * one's. Walking from there would put the new goroutine's frames under the old
 * goroutine's. */
static bool in_switch(uintptr_t pc) {
#if defined(BURROW_MCONTEXT_ASM)
    return pc - (uintptr_t)burrow__mcontext_switch_raw <
           burrow__mcontext_switch_raw_size;
#else
    (void)pc;
    return false;
#endif
}

/* Called if we receive a SIGPROF signal. Go's sigprof.
 *
 * The stack is the frames walked from the interrupted pc and frame pointer,
 * when the frame pointer is on the stack of the goroutine the thread is
 * running. When it is not, which is the scheduler running on the thread's own
 * stack or a goroutine in the middle of a switch, the sample is the pc and
 * _System, which is what Go records when it cannot walk the stack either.
 * Where the pc can't be read out of the context at all, the sample is just
 * _System. */
static void sigprof(void *ctx) {
    if (burrow__atomic_load_relaxed_u32(&prof_hz) == 0)
        return;

#if BURROW__ATOMIC_LOCK64
    /* Go checks whether the interrupted pc is in its atomics, on the
     * architectures where the 64 bit ones take a lock, because a write to the
     * log could need the lock that this thread is in the middle of holding.
     * burrow's atomics count the slots each thread holds, which says the same
     * thing without a symbol table. */
    if (burrow__atomic64_locked_here()) {
        (void)burrow__atomic_add_u32(&cpuprof.lost_atomic, 1);
        return;
    }
#endif

    uintptr_t pc = 0;
    uintptr_t fp = 0;
    bool have = pal_signal_context_pc_fp(ctx, &pc, &fp);

    burrow__M *mp = burrow__curm();
    if (mp == NULL) {
        /* sigprofNonGo: a thread the scheduler does not know about, and so
         * not one whose stack it knows the bounds of. */
        const uintptr_t stk[2] = {pc, MARKER_PC(burrow__prof_external_code)};
        if (have && pc != 0)
            cpuprof_add_non_go(stk, 2);
        else
            cpuprof_add_non_go(&stk[1], 1);
        return;
    }

    burrow__G *gp = mp->curg;
    uintptr_t stk[MAX_CPU_PROF_STACK];
    Int n = 0;
    if (have && in_switch(pc)) {
        /* Go's traceback stops at gogo, and so does this. */
        n = burrow__callers_from(pc, NULL, NULL, NULL, stk, MAX_CPU_PROF_STACK);
    } else if (have && gp != NULL && fp >= (uintptr_t)gp->stack.lo &&
               fp < (uintptr_t)gp->stack.hi)
        n = burrow__callers_from(pc, (void *)fp, gp->stack.lo, gp->stack.hi, stk,
                                 MAX_CPU_PROF_STACK);
    if (n <= 0) {
        /* Normal traceback is impossible or has failed. Account it against
         * abstract "System" code. */
        n = 2;
        stk[0] = pc;
        stk[1] = MARKER_PC(burrow__prof_system);
        if (!have || pc == 0) {
            /* Not in Go, which always knows the pc. Where this layer can't
             * read it out of the context there is only _System. */
            n = 1;
            stk[0] = stk[1];
        }
    }

    if (burrow__atomic_load_relaxed_u32(&prof_hz) != 0) {
        /* Note: it can happen on Windows that we interrupted a system thread
         * with no g, so gp could be NULL. The other nil checks are done out of
         * caution, but not expected to be NULL in practice. */
        void *const *tag_ptr = gp != NULL ? (void *const *)&gp->labels : NULL;
        cpuprof_add(tag_ptr, stk, n);
    }
}

/* The handler, which stays in once it is in. Not in Go: Go puts back the
 * handler that was there before when profiling stops, having first made a
 * default one into an ignore. A SIGPROF still on its way to a thread when the
 * timer stops is swallowed here instead, so the answer is the same and the
 * handler never changes under a signal in flight. */
static bool on_sigprof(int32_t sig, void *info, void *ctx) {
    (void)sig;
    (void)info;
    /* nanotime and the futex wake in the log keep errno to themselves, but the
     * interrupted code may be about to read it and the handler is cheap to
     * make sure of. */
    int saved = errno;
    sigprof(ctx);
    errno = saved;
    return true;
}

/* setProcessCPUProfiler is called when the profiling timer changes. Go's
 * setProcessCPUProfilerTimer, with the handler installed once rather than
 * swapped in and out. */
static void set_process_cpu_profiler(int32_t hz) {
    if (hz != 0) {
        /* Enable the signal handler if not enabled. */
        uint32_t no = 0;
        if (burrow__atomic_cas_u32(&prof_handler_installed, &no, 1)) {
            if (!pal_signal_install(PAL_SIGPROF, on_sigprof, NULL))
                burrow__atomic_store_release_u32(&prof_handler_installed, 0);
        }
        if (burrow__atomic_load_acquire_u32(&prof_handler_installed) == 0)
            return;
    }
    (void)pal_profile_timer(hz, NULL);
}

/* setcpuprofilerate sets the CPU profiling rate to hz times per second. If hz
 * <= 0, setcpuprofilerate turns off CPU profiling.
 *
 * Go turns off the calling thread's own timer while it holds the lock, so that
 * its signal cannot arrive on this thread and spin on the lock forever. There
 * is one timer for the whole process here, so SIGPROF is blocked on the
 * calling thread instead. */
static void set_cpu_profile_rate(int32_t hz) {
    if (hz < 0)
        hz = 0;

    (void)pal_signal_mask(PAL_SIGPROF, true, NULL);
    signal_lock();
    if ((int32_t)burrow__atomic_load_relaxed_u32(&prof_hz) != hz) {
        set_process_cpu_profiler(hz);
        burrow__atomic_store_release_u32(&prof_hz, (uint32_t)hz);
    }
    signal_unlock();
    (void)pal_signal_mask(PAL_SIGPROF, false, NULL);
}

void runtime_set_cpu_profile_rate(Int hz) {
    /* Clamp hz to something reasonable. */
    if (hz < 0)
        hz = 0;
    if (hz > 1000000)
        hz = 1000000;

    burrow__lock(&cpuprof.lock);
    if (hz > 0) {
        if (cpuprof.on || cpuprof.log != NULL) {
            fputs("runtime: cannot set cpu profile rate until previous profile has "
                  "finished.\n",
                  stderr);
            burrow__unlock(&cpuprof.lock);
            return;
        }

        burrow__ProfBuf *log =
            burrow__prof_buf_new(1, PROF_BUF_WORD_COUNT, PROF_BUF_TAG_COUNT);
        if (log == NULL)
            runtime_throw(BURROW_S("runtime: out of memory for the CPU profile"));
        cpuprof.on = true;
        cpuprof.log = log;
        const uint64_t hdr[1] = {(uint64_t)hz};
        burrow__prof_buf_write(cpuprof.log, NULL, burrow__nanotime(), hdr, 1, NULL, 0);
        set_cpu_profile_rate((int32_t)hz);
    } else if (cpuprof.on) {
        set_cpu_profile_rate(0);
        cpuprof.on = false;
        add_extra(&cpuprof);
        burrow__prof_buf_close(cpuprof.log);
    }
    burrow__unlock(&cpuprof.lock);
}

burrow__ProfBufRead burrow__read_cpu_profile(void) {
    burrow__lock(&cpuprof.lock);
    burrow__ProfBuf *log = cpuprof.log;
    burrow__unlock(&cpuprof.lock);

    /* Go reads blocking everywhere but darwin and ios, where a note is not
     * async-signal-safe. burrow's notes are not either wherever the futex
     * under them is a mutex and a condition variable, so it reads without
     * blocking everywhere, and runtime/pprof sleeps between reads as Go's does
     * on darwin. */
    burrow__ProfBufRead r = burrow__prof_buf_read(log, BURROW_PROF_BUF_NON_BLOCKING);
    if (r.ndata == 0 && r.eof && log != NULL) {
        burrow__lock(&cpuprof.lock);
        cpuprof.log = NULL;
        burrow__unlock(&cpuprof.lock);
        /* Not in Go, which leaves it to the collector. Nothing writes to a
         * closed log, and this was the last read of it. */
        burrow__prof_buf_free(log);
    }
    return r;
}

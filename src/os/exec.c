/* os.Process, os.ProcessState, os.StartProcess, os.FindProcess and
 * os.Executable.
 *
 * Derived from Go's src/os/exec.go.
 * Go source: go1.27.1.
 *
 * The rest is exec_posix.go, exec_unix.go, exec_windows.go and the
 * executable_*.go files, from the same release.
 *
 * Go keeps a pidfd for a child on Linux, and uses it to wait and to signal.
 * This does not yet, so a Unix process here is always what Go calls a PID
 * mode process, which is what Go falls back to on a Linux without pidfds and
 * what it uses on every other Unix. Windows keeps the process handle, as Go
 * does.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/sync.h"

#include "internal.h"

#include <string.h>

#define OS_LIT(s) str_from_bytes((const Byte *)(s), (Int)(sizeof(s) - 1))

BURROW_SENTINEL_ERROR(os_err_process_done, "os: process already finished");
BURROW_SENTINEL_ERROR(os_err_no_handle, "os: process handle unavailable");

static const Str os_exec_released_text = {(const Byte *)"os: process already released",
                                          28};
static const Error os_exec_released = {&burrow_sentinel_error_vt,
                                       &os_exec_released_text};

#if !defined(BURROW_OS_WINDOWS)
static const Str os_exec_unsupported_text = {
    (const Byte *)"os: unsupported signal type", 27};
static const Error os_exec_unsupported = {&burrow_sentinel_error_vt,
                                          &os_exec_unsupported_text};

static const Str os_exec_uninit_text = {(const Byte *)"os: process not initialized",
                                        27};
static const Error os_exec_uninit = {&burrow_sentinel_error_vt, &os_exec_uninit_text};
#endif

/* ----------------------------------------------------------------- signals */

/* Every number an OsSignal can carry, so that one can point at its own and
 * nothing is allocated. */
#define OS_NSIG 256
static const SyscallSignal os_signal_numbers[OS_NSIG] = {
#define OS_SIG8(n) (n), (n) + 1, (n) + 2, (n) + 3, (n) + 4, (n) + 5, (n) + 6, (n) + 7
#define OS_SIG64(n)                                                                    \
    OS_SIG8(n), OS_SIG8((n) + 8), OS_SIG8((n) + 16), OS_SIG8((n) + 24),                \
        OS_SIG8((n) + 32), OS_SIG8((n) + 40), OS_SIG8((n) + 48), OS_SIG8((n) + 56)
    OS_SIG64(0),
    OS_SIG64(64),
    OS_SIG64(128),
    OS_SIG64(192),
#undef OS_SIG64
#undef OS_SIG8
};

static Str os_signal_m_string(void *self, Alloc *a) {
    return syscall_signal_string(*(const SyscallSignal *)self, a);
}

static void os_signal_m_signal(void *self) {
    syscall_signal_signal(*(const SyscallSignal *)self);
}

static const OsSignalVT os_signal_vt = {
    &burrow__syscall_signal_desc,
    os_signal_m_string,
    os_signal_m_signal,
};

OsSignal os_signal_from_syscall(SyscallSignal s) {
    if (s < 0 || s >= OS_NSIG)
        return (OsSignal){NULL, NULL};
    return (OsSignal){&os_signal_vt, (void *)(Uintptr)&os_signal_numbers[s]};
}

SyscallSignal os_signal_to_syscall(OsSignal sig) {
    if (sig.vt != &os_signal_vt || sig.data == NULL)
        return -1;
    return *(const SyscallSignal *)sig.data;
}

const OsSignal os_interrupt = {&os_signal_vt,
                               (void *)(Uintptr)&os_signal_numbers[SYSCALL_SIGINT]};
const OsSignal os_kill = {&os_signal_vt,
                          (void *)(Uintptr)&os_signal_numbers[SYSCALL_SIGKILL]};

/* The descriptor for os.Signal itself, an interface, so that a channel of them
 * can be made for os/signal. Equality is Go's for an interface: the same
 * vtable and the same receiver, which for the signals above is the same number,
 * since each number has one slot to point at. */
static bool os_signal_ops_equal(const void *a, const void *b) {
    const OsSignal *x = (const OsSignal *)a;
    const OsSignal *y = (const OsSignal *)b;
    return x->vt == y->vt && x->data == y->data;
}

static uint64_t os_signal_ops_hash(const void *p, uint64_t seed) {
    const OsSignal *s = (const OsSignal *)p;
    uint64_t h = seed ^ 0x9e3779b97f4a7c15U;
    h = (h ^ (uint64_t)(Uintptr)s->vt) * 0x100000001b3U;
    h = (h ^ (uint64_t)(Uintptr)s->data) * 0x100000001b3U;
    return h;
}

static const TypeOps os_signal_ops = {
    os_signal_ops_equal,
    os_signal_ops_hash,
    NULL,
    NULL,
};

static const Type os_signal_type = {
    {(const Byte *)"Signal", 6},
    {(const Byte *)"os", 2},
    KIND_INTERFACE,
    (uint32_t)sizeof(OsSignal),
    (uint16_t)_Alignof(OsSignal),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6f736967U, /* "osig" */
    &os_signal_ops,
};

const Type *const TYPE_OS_SIGNAL = &os_signal_type;

/* ------------------------------------------------------------ the process */

enum { OS_STATUS_OK = 0, OS_STATUS_DONE = 1, OS_STATUS_RELEASED = 2 };

static OsProcess *os_process_new(Alloc *a, Int pid, bool has_handle, int64_t handle) {
    OsProcess *p = (OsProcess *)mem_alloc(a, sizeof *p, _Alignof(OsProcess));
    if (p == NULL)
        return NULL;
    p->pid = pid;
    p->alloc = a;
    p->has_handle = has_handle;
    p->handle = handle;
    sync_atomic_int32_store(&p->refs, has_handle ? 1 : 0);
    return p;
}

/* processHandle.release: the last one out closes it. */
static void os_handle_release(OsProcess *p) {
    for (;;) {
        int32_t refs = sync_atomic_int32_load(&p->refs);
        if (refs <= 0)
            panic_str(OS_LIT("internal error: too many releases of process handle"));
        if (sync_atomic_int32_compare_and_swap(&p->refs, refs, refs - 1)) {
            if (refs == 1)
                pal_process_close(p->handle);
            return;
        }
    }
}

/* processHandle.acquire. */
static bool os_handle_acquire(OsProcess *p) {
    for (;;) {
        int32_t refs = sync_atomic_int32_load(&p->refs);
        if (refs < 0)
            panic_str(
                OS_LIT("internal error: negative process handle reference count"));
        if (refs == 0)
            return false;
        if (sync_atomic_int32_compare_and_swap(&p->refs, refs, refs + 1))
            return true;
    }
}

/* handleTransientAcquire: the handle's status, holding a reference when it
 * is OS_STATUS_OK. */
static uint32_t os_handle_transient_acquire(OsProcess *p) {
    uint32_t status = sync_atomic_uint32_load(&p->state);
    if (status != OS_STATUS_OK)
        return status;
    if (os_handle_acquire(p))
        return OS_STATUS_OK;
    status = sync_atomic_uint32_load(&p->state);
    if (status == OS_STATUS_OK)
        panic_str(OS_LIT("inconsistent process status"));
    return status;
}

/* doRelease: moves an OK process to status and drops the handle, answering
 * the status it had. */
static uint32_t os_do_release(OsProcess *p, uint32_t status) {
    for (;;) {
        uint32_t old = sync_atomic_uint32_load(&p->state);
        if (old != OS_STATUS_OK)
            return old;
        if (!sync_atomic_uint32_compare_and_swap(&p->state, old, status))
            continue;
        if (p->has_handle)
            os_handle_release(p);
        return OS_STATUS_OK;
    }
}

Error os_process_release(OsProcess *p) {
#if defined(BURROW_OS_WINDOWS)
    if (os_do_release(p, OS_STATUS_RELEASED) == OS_STATUS_RELEASED)
        return burrow__os_errno_value(SYSCALL_EINVAL);
#else
    p->pid = -1;
    (void)os_do_release(p, OS_STATUS_RELEASED);
#endif
    return BURROW_NO_ERROR;
}

void os_process_free(OsProcess *p) {
    if (p == NULL)
        return;
    (void)os_do_release(p, OS_STATUS_RELEASED);
    mem_free(p->alloc, p, sizeof *p, _Alignof(OsProcess));
}

Error os_process_kill(OsProcess *p) {
    return os_process_signal(p, os_kill);
}

Error os_process_with_handle(OsProcess *p, OsHandleFunc f) {
    if (!p->has_handle)
        return os_err_no_handle;
    switch (os_handle_transient_acquire(p)) {
    case OS_STATUS_DONE:
        return os_err_process_done;
    case OS_STATUS_RELEASED:
        return os_exec_released;
    default:
        break;
    }
    f.f(f.env, (Uintptr)p->handle);
    os_handle_release(p);
    return BURROW_NO_ERROR;
}

static OsProcessState *os_state_new(OsProcess *p, Int pid, SyscallWaitStatus st,
                                    const PalRusage *ru, Error *err) {
    OsProcessState *ps =
        (OsProcessState *)mem_alloc(p->alloc, sizeof *ps, _Alignof(OsProcessState));
    if (ps == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    ps->pid = pid;
    ps->status = st;
    ps->rusage = syscall_rusage_from_pal(ru);
    ps->alloc = p->alloc;
    return ps;
}

#if defined(BURROW_OS_WINDOWS)

OsProcessState *os_process_wait(OsProcess *p, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    switch (os_handle_transient_acquire(p)) {
    case OS_STATUS_DONE:
        BURROW_OUT(err, os_err_process_done);
        return NULL;
    case OS_STATUS_RELEASED:
        BURROW_OUT(err, burrow__os_errno_value(SYSCALL_EINVAL));
        return NULL;
    default:
        break;
    }
    PalErrno pe = PAL_OK;
    uint32_t code = 0;
    PalRusage ru;
    int64_t r = pal_wait4(p->handle, &code, 0, &ru, &pe);
    os_handle_release(p);
    if (r < 0) {
        BURROW_OUT(err, os_new_syscall_error(error_allocator(),
                                             OS_LIT("WaitForSingleObject"),
                                             burrow__os_errno(pe)));
        return NULL;
    }
    (void)os_do_release(p, OS_STATUS_RELEASED);
    SyscallWaitStatus st = {code};
    return os_state_new(p, p->pid, st, &ru, err);
}

Error os_process_signal(OsProcess *p, OsSignal sig) {
    switch (os_handle_transient_acquire(p)) {
    case OS_STATUS_DONE:
        return os_err_process_done;
    case OS_STATUS_RELEASED:
        return burrow__os_errno_value(SYSCALL_EINVAL);
    default:
        break;
    }
    Error e = BURROW_NO_ERROR;
    if (os_signal_to_syscall(sig) == SYSCALL_SIGKILL) {
        PalErrno pe = PAL_OK;
        if (!pal_kill_native(p->handle, 9, &pe))
            e = os_new_syscall_error(error_allocator(), OS_LIT("TerminateProcess"),
                                     burrow__os_errno(pe));
    } else {
        e = burrow__os_errno_value(SYSCALL_EWINDOWS);
    }
    os_handle_release(p);
    return e;
}

OsProcess *os_find_process(Alloc *a, Int pid, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    PalErrno pe = PAL_OK;
    int64_t h = pal_process_open((int64_t)pid, &pe);
    if (h < 0) {
        BURROW_OUT(err, os_new_syscall_error(error_allocator(), OS_LIT("OpenProcess"),
                                             burrow__os_errno(pe)));
        return NULL;
    }
    OsProcess *p = os_process_new(a, pid, true, h);
    if (p == NULL) {
        pal_process_close(h);
        BURROW_OUT(err, burrow_err_out_of_memory);
    }
    return p;
}

#else

OsProcessState *os_process_wait(OsProcess *p, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (sync_atomic_uint32_load(&p->state) == OS_STATUS_RELEASED) {
        BURROW_OUT(err, burrow__os_errno_value(SYSCALL_EINVAL));
        return NULL;
    }
    /* blockUntilWaitable: once the child can be reaped, stop anyone signalling
     * it, and wait for a signal already on its way, so that nothing is sent
     * to whatever gets its pid next. */
    PalErrno pe = PAL_OK;
    if (pal_wait_ready((int64_t)p->pid, &pe)) {
        (void)os_do_release(p, OS_STATUS_DONE);
        sync_rw_mutex_lock(&p->sig_mu);
        sync_rw_mutex_unlock(&p->sig_mu);
    } else if (pe != PAL_ENOTSUP) {
#if defined(BURROW_OS_LINUX)
        Str op = OS_LIT("waitid");
#else
        Str op = OS_LIT("wait6");
#endif
        BURROW_OUT(err,
                   os_new_syscall_error(error_allocator(), op, burrow__os_errno(pe)));
        return NULL;
    }
    uint32_t st = 0;
    PalRusage ru;
    int64_t got;
    do {
        pe = PAL_OK;
        got = pal_wait4((int64_t)p->pid, &st, 0, &ru, &pe);
    } while (got < 0 && pe == PAL_EINTR);
    if (got < 0) {
        BURROW_OUT(err, os_new_syscall_error(error_allocator(), OS_LIT("wait"),
                                             burrow__os_errno(pe)));
        return NULL;
    }
    (void)os_do_release(p, OS_STATUS_DONE);
    return os_state_new(p, (Int)got, (SyscallWaitStatus)st, &ru, err);
}

Error os_process_signal(OsProcess *p, OsSignal sig) {
    SyscallSignal s = os_signal_to_syscall(sig);
    if (s < 0)
        return os_exec_unsupported;
    if (p->pid == -1)
        return os_exec_released;
    if (p->pid == 0)
        return os_exec_uninit;
    sync_rw_mutex_r_lock(&p->sig_mu);
    Error e = BURROW_NO_ERROR;
    switch (sync_atomic_uint32_load(&p->state)) {
    case OS_STATUS_DONE:
        e = os_err_process_done;
        break;
    case OS_STATUS_RELEASED:
        e = os_exec_released;
        break;
    default: {
        PalErrno pe = PAL_OK;
        if (!pal_kill_native((int64_t)p->pid, (int32_t)s, &pe))
            e = pe == PAL_ESRCH ? os_err_process_done : burrow__os_errno(pe);
        break;
    }
    }
    sync_rw_mutex_r_unlock(&p->sig_mu);
    return e;
}

OsProcess *os_find_process(Alloc *a, Int pid, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    OsProcess *p = os_process_new(a, pid, false, 0);
    if (p == NULL)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return p;
}

#endif

/* ------------------------------------------------------------ starting one */

/* The C strings a spawn needs, in one block from the heap. */
typedef struct OsSpawnArgs {
    char *block;
    size_t size;
    const char **argv;
    const char **envp;
    const char *path;
    const char *dir;
    int64_t *fds;
    int32_t nfds;
} OsSpawnArgs;

static bool os_has_nul(Str s) {
    return s.len > 0 && memchr(s.p, 0, (size_t)s.len) != NULL;
}

/* Copies s and a NUL to *at and moves it on. */
static const char *os_spawn_put(char **at, Str s) {
    char *out = *at;
    if (s.len > 0)
        memcpy(out, s.p, (size_t)s.len);
    out[s.len] = 0;
    *at = out + s.len + 1;
    return out;
}

/* Lays out name, argv, env and dir. False with *e set to EINVAL when one of
 * them has a NUL in it, as Go's BytePtrFromString says, or to out of memory. */
static bool os_spawn_args(OsSpawnArgs *sa, Str name, Slice argv, Slice env, Str dir,
                          Slice files, Error *e) {
    memset(sa, 0, sizeof *sa);
    const Str *av = (const Str *)argv.p;
    const Str *ev = (const Str *)env.p;
    size_t bytes = (size_t)name.len + 1 + (size_t)dir.len + 1;
    if (os_has_nul(name) || os_has_nul(dir))
        goto inval;
    for (Int i = 0; i < argv.len; i++) {
        if (os_has_nul(av[i]))
            goto inval;
        bytes += (size_t)av[i].len + 1;
    }
    for (Int i = 0; i < env.len; i++) {
        if (os_has_nul(ev[i]))
            goto inval;
        bytes += (size_t)ev[i].len + 1;
    }
    size_t ptrs = ((size_t)argv.len + 1 + (size_t)env.len + 1) * sizeof(char *);
    size_t nfd = (size_t)files.len;
    sa->size = ptrs + nfd * sizeof(int64_t) + bytes;
    sa->block = (char *)mem_alloc_nozero(heap_allocator(), sa->size, _Alignof(int64_t));
    if (sa->block == NULL) {
        BURROW_OUT(e, burrow_err_out_of_memory);
        return false;
    }
    sa->fds = (int64_t *)(void *)sa->block;
    sa->argv = (const char **)(void *)(sa->block + nfd * sizeof(int64_t));
    sa->envp = sa->argv + argv.len + 1;
    char *at = sa->block + nfd * sizeof(int64_t) + ptrs;
    sa->path = os_spawn_put(&at, name);
    sa->dir = os_spawn_put(&at, dir);
    for (Int i = 0; i < argv.len; i++)
        sa->argv[i] = os_spawn_put(&at, av[i]);
    sa->argv[argv.len] = NULL;
    for (Int i = 0; i < env.len; i++)
        sa->envp[i] = os_spawn_put(&at, ev[i]);
    sa->envp[env.len] = NULL;
    OsFile *const *fv = (OsFile *const *)files.p;
    for (size_t i = 0; i < nfd; i++)
        sa->fds[i] = fv[i] == NULL ? PAL_INVALID_HANDLE : (int64_t)os_file_fd(fv[i]);
    sa->nfds = (int32_t)nfd;
    return true;
inval:
    BURROW_OUT(e, burrow__os_errno_value(SYSCALL_EINVAL));
    return false;
}

static void os_spawn_args_free(OsSpawnArgs *sa) {
    if (sa->block != NULL)
        mem_free(heap_allocator(), sa->block, sa->size, _Alignof(int64_t));
}

static OsProcess *os_start_fail(Str name, Error e, Error *err) {
    BURROW_OUT(err, fs_path_error_new(error_allocator(), OS_LIT("fork/exec"), name, e));
    return NULL;
}

OsProcess *os_start_process(Alloc *a, Str name, Slice argv, const OsProcAttr *attr,
                            Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    static const OsProcAttr none = {
        {NULL, 0}, {NULL, 0, 0, NULL}, {NULL, 0, 0, NULL}, NULL};
    if (attr == NULL)
        attr = &none;

    /* A Dir that is not there is a chdir error rather than a fork/exec one,
     * found before anything is started. */
    if (attr->sys == NULL && attr->dir.len > 0) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Error se = BURROW_NO_ERROR;
        (void)os_stat(arena_allocator(&ar), attr->dir, &se);
        arena_free(&ar);
        if (BURROW_FAILED(se)) {
            const FsPathError *pe =
                (const FsPathError *)errors_as(se, TYPE_FS_PATH_ERROR);
            if (pe != NULL)
                se = fs_path_error_new(error_allocator(), OS_LIT("chdir"), pe->path,
                                       pe->err);
            BURROW_OUT(err, se);
            return NULL;
        }
    }

    uint32_t flags = 0;
    uint32_t creation_flags = 0;
#if defined(BURROW_OS_WINDOWS)
    if (attr->sys != NULL)
        creation_flags = attr->sys->creation_flags;
    if (name.len == 0)
        return os_start_fail(name, burrow__os_errno_value(SYSCALL_EWINDOWS), err);
#endif

    Arena ar;
    arena_init(&ar, NULL, 0);
#if !defined(BURROW_OS_WINDOWS)
    /* What the child does before the exec, as syscall's StartProcess would
     * take it, in memory that lasts until the spawn is done. */
    Arena sysar;
    arena_init(&sysar, NULL, 0);
    PalSpawnSys sys;
    Error se = BURROW_NO_ERROR;
    if (!burrow__syscall_spawn_sys(attr->sys, attr->files.len, arena_allocator(&sysar),
                                   &sys, &flags, &se)) {
        arena_free(&sysar);
        arena_free(&ar);
        return os_start_fail(name, se, err);
    }
#endif
    Slice env = attr->env;
    if (env.p == NULL)
        env = os_environ(arena_allocator(&ar));

    OsSpawnArgs sa;
    Error e = BURROW_NO_ERROR;
    if (!os_spawn_args(&sa, name, argv, env, attr->dir, attr->files, &e)) {
        arena_free(&ar);
#if !defined(BURROW_OS_WINDOWS)
        arena_free(&sysar);
#endif
        return os_start_fail(name, e, err);
    }
    arena_free(&ar);

    PalSpawn req;
    memset(&req, 0, sizeof req);
    req.path = sa.path;
    req.argv = sa.argv;
    req.envp = sa.envp;
    req.dir = attr->dir.len > 0 ? sa.dir : NULL;
    req.fds = sa.fds;
    req.nfds = sa.nfds;
    req.flags = flags;
    req.creation_flags = creation_flags;
    PalErrno pe = PAL_OK;
#if defined(BURROW_OS_WINDOWS)
    int64_t pid = pal_spawn(&req, &pe);
#else
    req.sys = &sys;
    sync_rw_mutex_lock(&syscall_fork_lock);
    int64_t pid = pal_spawn(&req, &pe);
    sync_rw_mutex_unlock(&syscall_fork_lock);
#if defined(BURROW_OS_LINUX)
    if (sys.pidfd != NULL && attr->sys != NULL)
        *attr->sys->pid_fd = (Int)*sys.pidfd;
#endif
    arena_free(&sysar);
#endif
    os_spawn_args_free(&sa);
    if (pid < 0)
        return os_start_fail(name, burrow__os_errno(pe), err);

#if defined(BURROW_OS_WINDOWS)
    OsProcess *p = os_process_new(a, (Int)pal_process_id(pid), true, pid);
    if (p == NULL)
        pal_process_close(pid);
#else
    OsProcess *p = os_process_new(a, (Int)pid, false, 0);
#endif
    if (p == NULL)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return p;
}

/* ---------------------------------------------------------- ProcessState */

Int os_process_state_pid(const OsProcessState *ps) {
    return ps->pid;
}

bool os_process_state_exited(const OsProcessState *ps) {
    return syscall_wait_status_exited(ps->status);
}

bool os_process_state_success(const OsProcessState *ps) {
    return syscall_wait_status_exit_status(ps->status) == 0;
}

Int os_process_state_exit_code(const OsProcessState *ps) {
    if (ps == NULL)
        return -1;
    return syscall_wait_status_exit_status(ps->status);
}

SyscallWaitStatus os_process_state_sys(const OsProcessState *ps) {
    return ps->status;
}

const SyscallRusage *os_process_state_sys_usage(const OsProcessState *ps) {
    return &ps->rusage;
}

#if defined(BURROW_OS_WINDOWS)
/* ftToDuration: an amount of time, not a date, so there is no epoch. */
static Duration os_ft_duration(const SyscallFiletime *ft) {
    uint64_t n = ((uint64_t)ft->high_date_time << 32) + (uint64_t)ft->low_date_time;
    return (Duration)(n * 100u);
}

Duration os_process_state_user_time(const OsProcessState *ps) {
    return os_ft_duration(&ps->rusage.user_time);
}

Duration os_process_state_system_time(const OsProcessState *ps) {
    return os_ft_duration(&ps->rusage.kernel_time);
}
#else
Duration os_process_state_user_time(const OsProcessState *ps) {
    return (Duration)syscall_timeval_nano(&ps->rusage.utime);
}

Duration os_process_state_system_time(const OsProcessState *ps) {
    return (Duration)syscall_timeval_nano(&ps->rusage.stime);
}
#endif

void os_process_state_free(OsProcessState *ps) {
    if (ps != NULL)
        mem_free(ps->alloc, ps, sizeof *ps, _Alignof(OsProcessState));
}

/* Appends s to buf at *n, which has room. */
static void os_ps_put(char *buf, Int *n, Str s) {
    memcpy(buf + *n, s.p, (size_t)s.len);
    *n += s.len;
}

/* Appends v in base 10, or base 16 with hex. */
static void os_ps_num(char *buf, Int *n, uint64_t v, bool neg, bool hex) {
    char digits[24];
    int nd = 0;
    unsigned base = hex ? 16U : 10U;
    do {
        digits[nd++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v != 0);
    if (neg)
        buf[(*n)++] = '-';
    while (nd > 0)
        buf[(*n)++] = digits[--nd];
}

static void os_ps_int(char *buf, Int *n, Int v) {
    os_ps_num(buf, n, v < 0 ? 0 - (uint64_t)v : (uint64_t)v, v < 0, false);
}

Str os_process_state_string(const OsProcessState *ps, Alloc *a) {
    if (ps == NULL)
        return burrow__os_cat3(a, OS_LIT("<nil>"), (Str){NULL, 0}, (Str){NULL, 0});
    SyscallWaitStatus st = ps->status;
    char buf[160];
    Int n = 0;
    Arena ar;
    arena_init(&ar, NULL, 0);
    if (syscall_wait_status_exited(st)) {
        Int code = syscall_wait_status_exit_status(st);
#if defined(BURROW_OS_WINDOWS)
        if ((uint64_t)code >= ((uint64_t)1 << 16)) {
            os_ps_put(buf, &n, OS_LIT("exit status 0x"));
            os_ps_num(buf, &n, (uint64_t)code, false, true);
        } else
#endif
        {
            os_ps_put(buf, &n, OS_LIT("exit status "));
            os_ps_int(buf, &n, code);
        }
    } else if (syscall_wait_status_signaled(st)) {
        Str name =
            syscall_signal_string(syscall_wait_status_signal(st), arena_allocator(&ar));
        os_ps_put(buf, &n, OS_LIT("signal: "));
        if (name.len > 100)
            name.len = 100;
        os_ps_put(buf, &n, name);
    } else if (syscall_wait_status_stopped(st)) {
        SyscallSignal sig = syscall_wait_status_stop_signal(st);
        Str name = syscall_signal_string(sig, arena_allocator(&ar));
        os_ps_put(buf, &n, OS_LIT("stop signal: "));
        if (name.len > 80)
            name.len = 80;
        os_ps_put(buf, &n, name);
        Int cause = syscall_wait_status_trap_cause(st);
        if (sig == SYSCALL_SIGTRAP && cause != 0) {
            os_ps_put(buf, &n, OS_LIT(" (trap "));
            os_ps_int(buf, &n, cause);
            os_ps_put(buf, &n, OS_LIT(")"));
        }
    } else if (syscall_wait_status_continued(st)) {
        os_ps_put(buf, &n, OS_LIT("continued"));
    }
    if (syscall_wait_status_core_dump(st))
        os_ps_put(buf, &n, OS_LIT(" (core dumped)"));
    arena_free(&ar);
    return burrow__os_cat3(a, str_from_bytes((const Byte *)buf, n), (Str){NULL, 0},
                           (Str){NULL, 0});
}

/* -------------------------------------------------------------- Executable */

Str os_executable(Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_ANDROID)
    Str path = os_readlink(a, OS_LIT("/proc/self/exe"), err);
    Str deleted = OS_LIT(" (deleted)");
    if (path.len >= deleted.len &&
        memcmp(path.p + path.len - deleted.len, deleted.p, (size_t)deleted.len) == 0)
        path.len -= deleted.len;
    return path;
#else
    char small[1024];
    char *buf = small;
    int64_t cap = (int64_t)sizeof small;
    PalErrno pe = PAL_OK;
    int64_t n = pal_executable(buf, cap, &pe);
    if (n < 0 && pe == PAL_ERANGE) {
        cap = 65536;
        buf = (char *)mem_alloc_nozero(heap_allocator(), (size_t)cap, 1);
        if (buf == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return (Str){NULL, 0};
        }
        n = pal_executable(buf, cap, &pe);
    }
    Str out = {NULL, 0};
    if (n < 0 && pe == PAL_ENOTSUP) {
        BURROW_OUT(
            err, errors_new(error_allocator(),
                            OS_LIT("Executable not implemented for " BURROW_OS_NAME)));
    } else if (n < 0) {
        BURROW_OUT(err, burrow__os_errno(pe));
    } else if (n == 0) {
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
        BURROW_OUT(
            err, errors_new(error_allocator(), OS_LIT("cannot find executable path")));
#else
        out = OS_LIT("");
#endif
    } else {
        Str ep = str_from_bytes((const Byte *)buf, (Int)n);
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || defined(BURROW_OS_SOLARIS)
        /* A relative path is from the directory the program started in, which
         * Go reads at start up and this reads now. */
        if (ep.p[0] != '/') {
            Arena ar;
            arena_init(&ar, NULL, 0);
            Error we = BURROW_NO_ERROR;
            Str wd = os_getwd(arena_allocator(&ar), &we);
            if (BURROW_FAILED(we)) {
                BURROW_OUT(err, we);
                out = burrow__os_cat3(a, ep, (Str){NULL, 0}, (Str){NULL, 0});
            } else {
                if (ep.len > 2 && ep.p[0] == '.' && ep.p[1] == '/') {
                    ep.p += 2;
                    ep.len -= 2;
                }
                out = burrow__os_cat3(a, wd, OS_LIT("/"), ep);
            }
            arena_free(&ar);
        } else
#endif
        {
            out = burrow__os_cat3(a, ep, (Str){NULL, 0}, (Str){NULL, 0});
        }
        if (out.p == NULL && ep.len > 0)
            BURROW_OUT(err, burrow_err_out_of_memory);
    }
    if (buf != small)
        mem_free(heap_allocator(), buf, (size_t)cap, 1);
    return out;
#endif
}

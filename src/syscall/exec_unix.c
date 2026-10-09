/* ForkExec, StartProcess, Exec and ForkLock, the set id family Go writes by
 * hand on Linux, Setgroups, and the SysProcAttr checks os shares. The fork
 * itself is pal_spawn's, which does in the child what Go's forkAndExecInChild
 * does, in the same order.
 *
 * Derived from Go's src/syscall/exec_unix.go, exec_linux.go, exec_libc2.go,
 * exec_freebsd.go and syscall_linux.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include "internal.h"

#include <stdint.h>
#include <string.h>

SyncRWMutex syscall_fork_lock;

static Error exec_errno(SyscallErrno e) {
    return burrow__syscall_errno_err(e);
}

static Error exec_pal_err(PalErrno pe) {
    return burrow__syscall_errno_err(syscall_errno_from_pal(pe));
}

/* s as a C string in a. NULL with EINVAL in *err if s has a NUL in it, as
 * BytePtrFromString does. */
static char *exec_cstring(Alloc *a, Str s, Error *err) {
    if (s.len > 0 && memchr(s.p, 0, (size_t)s.len) != NULL) {
        BURROW_OUT(err, exec_errno(SYSCALL_EINVAL));
        return NULL;
    }
    char *p = (char *)mem_alloc_nozero(a, (size_t)s.len + 1, 1);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    p[s.len] = 0;
    return p;
}

/* A slice of Str as an array of C strings with a NULL after the last, in a,
 * which is SlicePtrFromStrings. */
static const char **exec_cstrings(Alloc *a, Slice strs, Error *err) {
    const char **v = BURROW_NEW_N(a, const char *, (size_t)strs.len + 1);
    if (v == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    const Str *sv = (const Str *)strs.p;
    for (Int i = 0; i < strs.len; i++) {
        v[i] = exec_cstring(a, sv[i], err);
        if (v[i] == NULL)
            return NULL;
    }
    v[strs.len] = NULL;
    return v;
}

#if defined(BURROW_OS_LINUX)
/* Appends the decimal for v at *at. */
static void exec_put_int(char **at, Int v) {
    char digits[24];
    int n = 0;
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    do {
        digits[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (v < 0)
        *(*at)++ = '-';
    while (n > 0)
        *(*at)++ = digits[--n];
}

/* Go's formatIDMappings: one "container host size" line for each map, or
 * NULL for a nil slice, which is different from an empty one. */
static const char *exec_id_map(Alloc *a, Slice maps, Error *err) {
    if (maps.p == NULL)
        return NULL;
    const SyscallSysProcIDMap *m = (const SyscallSysProcIDMap *)maps.p;
    /* Three numbers of at most 20 digits and a sign, two spaces and a
     * newline. */
    char *text = (char *)mem_alloc_nozero(a, (size_t)maps.len * 66 + 1, 1);
    if (text == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    char *at = text;
    for (Int i = 0; i < maps.len; i++) {
        exec_put_int(&at, m[i].container_id);
        *at++ = ' ';
        exec_put_int(&at, m[i].host_id);
        *at++ = ' ';
        exec_put_int(&at, m[i].size);
        *at++ = '\n';
    }
    *at = 0;
    return text;
}
#endif

bool burrow__syscall_spawn_sys(const SyscallSysProcAttr *sys, Int nfiles, Alloc *a,
                               PalSpawnSys *out, uint32_t *flags, Error *err) {
    memset(out, 0, sizeof *out);
    *flags = 0;
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (sys == NULL)
        return true;

    /* Both use ctty, but setctty means a descriptor in the child and
     * foreground one in the parent. */
    if (sys->setctty && sys->foreground) {
        BURROW_OUT(
            err,
            errors_new(error_allocator(),
                       BURROW_S("both Setctty and Foreground set in SysProcAttr")));
        return false;
    }
    if (sys->setctty && sys->ctty >= nfiles) {
        BURROW_OUT(err,
                   errors_new(error_allocator(),
                              BURROW_S("Setctty set but Ctty not valid in child")));
        return false;
    }

    if (sys->setsid)
        *flags |= PAL_SPAWN_SETSID;
    if (sys->setpgid)
        *flags |= PAL_SPAWN_SETPGID;
    out->pgid = (int64_t)sys->pgid;
    out->foreground = sys->foreground;
    out->setctty = sys->setctty;
    out->noctty = sys->noctty;
    out->ctty = (int64_t)sys->ctty;
    out->ptrace = sys->ptrace;
    if (sys->chroot.len > 0) {
        out->chroot = exec_cstring(a, sys->chroot, err);
        if (out->chroot == NULL)
            return false;
    }
    if (sys->credential != NULL) {
        const SyscallCredential *cred = sys->credential;
        out->credential = true;
        out->uid = cred->uid;
        out->gid = cred->gid;
        out->groups = (const uint32_t *)cred->groups.p;
        out->ngroups = (int64_t)cred->groups.len;
        out->no_set_groups = cred->no_set_groups;
    }

#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_FREEBSD)
    out->pdeathsig = (int32_t)sys->pdeathsig;
#endif
#if defined(BURROW_OS_FREEBSD)
    out->jail = (int64_t)sys->jail;
#endif
#if defined(BURROW_OS_LINUX)
    out->cloneflags = (uint64_t)sys->cloneflags;
    out->unshareflags = (uint64_t)sys->unshareflags;
    Error e = BURROW_NO_ERROR;
    out->uid_map = exec_id_map(a, sys->uid_mappings, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return false;
    }
    out->gid_map = exec_id_map(a, sys->gid_mappings, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return false;
    }
    out->gid_map_setgroups = sys->gid_mappings_enable_setgroups;
    if (sys->ambient_caps.len > 0) {
        uint64_t *caps = BURROW_NEW_N(a, uint64_t, (size_t)sys->ambient_caps.len);
        if (caps == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return false;
        }
        const Uintptr *cv = (const Uintptr *)sys->ambient_caps.p;
        for (Int i = 0; i < sys->ambient_caps.len; i++)
            caps[i] = (uint64_t)cv[i];
        out->ambient_caps = caps;
        out->nambient_caps = (int64_t)sys->ambient_caps.len;
    }
    out->use_cgroup_fd = sys->use_cgroup_fd;
    out->cgroup_fd = (int64_t)sys->cgroup_fd;
    if (sys->pid_fd != NULL) {
        /* The kernel writes an int, and *pid_fd is only changed if the fork
         * happens, so the slot starts out as what is there now. */
        int32_t *slot = BURROW_NEW_N(a, int32_t, 1);
        if (slot == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return false;
        }
        *slot = (int32_t)*sys->pid_fd;
        out->pidfd = slot;
    }
#endif
    return true;
}

Int syscall_fork_exec(Str argv0, Slice argv, const SyscallProcAttr *attr, Error *err) {
    static const SyscallProcAttr none;
    if (attr == NULL)
        attr = &none;
    BURROW_OUT(err, BURROW_NO_ERROR);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error e = BURROW_NO_ERROR;
    Int pid = 0;
    PalSpawn req;
    memset(&req, 0, sizeof req);
    PalSpawnSys sys;
    uint32_t flags = 0;

    req.path = exec_cstring(a, argv0, &e);
    if (req.path == NULL)
        goto done;
    const char **args = exec_cstrings(a, argv, &e);
    if (args == NULL)
        goto done;
    req.argv = args;
    /* A nil env is no environment at all here, where os would give ours. */
    req.envp = exec_cstrings(a, attr->env, &e);
    if (req.envp == NULL)
        goto done;
#if defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_DRAGONFLY)
    /* What Go does there, for the kernel's sake. */
    if (argv.len > 0 && ((const Str *)argv.p)[0].len > argv0.len)
        args[0] = req.path;
#endif
    if (attr->dir.len > 0) {
        req.dir = exec_cstring(a, attr->dir, &e);
        if (req.dir == NULL)
            goto done;
    }
    if (!burrow__syscall_spawn_sys(attr->sys, attr->files.len, a, &sys, &flags, &e))
        goto done;
    if (attr->files.len > INT32_MAX) {
        e = exec_errno(SYSCALL_EINVAL);
        goto done;
    }
    int64_t *fds = BURROW_NEW_N(a, int64_t, (size_t)attr->files.len + 1);
    if (fds == NULL) {
        e = burrow_err_out_of_memory;
        goto done;
    }
    const Uintptr *fv = (const Uintptr *)attr->files.p;
    for (Int i = 0; i < attr->files.len; i++) {
        /* int(ufd) in Go, so ^uintptr(0) is -1 and leaves the slot closed,
         * and anything too big to be a descriptor fails in the child with
         * EBADF as it does there. */
        if (fv[i] == ~(Uintptr)0)
            fds[i] = PAL_INVALID_HANDLE;
        else
            fds[i] = fv[i] > INT32_MAX ? INT32_MAX : (int64_t)fv[i];
    }
    req.fds = fds;
    req.nfds = (int32_t)attr->files.len;
    req.flags = flags;
    /* Go leaves open whatever is not close on exec, and so does this. */
    sys.keep_fds = true;
    req.sys = &sys;

    PalErrno pe = PAL_OK;
    sync_rw_mutex_lock(&syscall_fork_lock);
    int64_t got = pal_spawn(&req, &pe);
    sync_rw_mutex_unlock(&syscall_fork_lock);
#if defined(BURROW_OS_LINUX)
    if (sys.pidfd != NULL)
        *attr->sys->pid_fd = (Int)*sys.pidfd;
#endif
    if (got < 0)
        e = exec_pal_err(pe);
    else
        pid = (Int)got;

done:
    arena_free(&ar);
    BURROW_OUT(err, e);
    return pid;
}

Int syscall_start_process(Str argv0, Slice argv, const SyscallProcAttr *attr,
                          Uintptr *handle, Error *err) {
    BURROW_OUT(handle, 0);
    return syscall_fork_exec(argv0, argv, attr, err);
}

Error syscall_exec(Str argv0, Slice argv, Slice envv) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error e = BURROW_NO_ERROR;
    char *path = exec_cstring(a, argv0, &e);
    const char **av = path != NULL ? exec_cstrings(a, argv, &e) : NULL;
    const char **ev = av != NULL ? exec_cstrings(a, envv, &e) : NULL;
    if (ev != NULL) {
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
        e = burrow__syscall_execve((uint8_t *)path, (uint8_t **)(void *)av,
                                   (uint8_t **)(void *)ev);
#elif defined(SYSCALL_SYS_EXECVE)
        SyscallErrno e1 = 0;
        syscall_raw_syscall(SYSCALL_SYS_EXECVE, (Uintptr)(void *)path,
                            (Uintptr)(void *)av, (Uintptr)(void *)ev, NULL, &e1);
        e = exec_errno(e1);
#else
        e = exec_errno(SYSCALL_ENOSYS);
#endif
    }
    arena_free(&ar);
    return e;
}

Error syscall_setgroups(Slice gids) {
    uint32_t small[32];
    uint32_t *list = small;
    if (gids.len > (Int)(sizeof small / sizeof small[0])) {
        list = BURROW_NEW_N(heap_allocator(), uint32_t, (size_t)gids.len);
        if (list == NULL)
            return burrow_err_out_of_memory;
    }
    const Int *gv = (const Int *)gids.p;
    for (Int i = 0; i < gids.len; i++)
        list[i] = (uint32_t)gv[i];
    PalErrno pe = PAL_OK;
    Error e = BURROW_NO_ERROR;
    if (!pal_setgroups(list, (int64_t)gids.len, &pe))
        e = exec_pal_err(pe);
    if (list != small)
        mem_free(heap_allocator(), list, (size_t)gids.len * sizeof *list,
                 _Alignof(uint32_t));
    return e;
}

#if defined(BURROW_OS_LINUX)
static Error exec_set_ids(PalSetID which, Int a, Int b, Int c) {
    PalErrno pe = PAL_OK;
    if (!pal_set_ids(which, (uint32_t)a, (uint32_t)b, (uint32_t)c, &pe))
        return exec_pal_err(pe);
    return BURROW_NO_ERROR;
}

Error syscall_setuid(Int uid) {
    return exec_set_ids(PAL_SETUID, uid, 0, 0);
}

Error syscall_setgid(Int gid) {
    return exec_set_ids(PAL_SETGID, gid, 0, 0);
}

Error syscall_seteuid(Int euid) {
    return exec_set_ids(PAL_SETEUID, euid, 0, 0);
}

Error syscall_setegid(Int egid) {
    return exec_set_ids(PAL_SETEGID, egid, 0, 0);
}

Error syscall_setreuid(Int ruid, Int euid) {
    return exec_set_ids(PAL_SETREUID, ruid, euid, 0);
}

Error syscall_setregid(Int rgid, Int egid) {
    return exec_set_ids(PAL_SETREGID, rgid, egid, 0);
}

Error syscall_setresuid(Int ruid, Int euid, Int suid) {
    return exec_set_ids(PAL_SETRESUID, ruid, euid, suid);
}

Error syscall_setresgid(Int rgid, Int egid, Int sgid) {
    return exec_set_ids(PAL_SETRESGID, rgid, egid, sgid);
}

Uintptr syscall_all_threads_syscall(Uintptr trap, Uintptr a1, Uintptr a2, Uintptr a3,
                                    Uintptr *r2, SyscallErrno *err) {
    return syscall_all_threads_syscall6(trap, a1, a2, a3, 0, 0, 0, r2, err);
}

Uintptr syscall_all_threads_syscall6(Uintptr trap, Uintptr a1, Uintptr a2, Uintptr a3,
                                     Uintptr a4, Uintptr a5, Uintptr a6, Uintptr *r2,
                                     SyscallErrno *err) {
    (void)trap;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    BURROW_OUT(r2, ~(Uintptr)0);
    BURROW_OUT(err, SYSCALL_ENOTSUP);
    return ~(Uintptr)0;
}
#endif

#endif

/* os.Args, the process's ids, os.Exit, os.Getwd, os.Chdir, os.Hostname and
 * the user's directories.
 *
 * Derived from Go's src/os/proc.go.
 * Go source: go1.27.1.
 *
 * The rest is getwd.go, file.go's Chdir and user directories, exec.go's
 * Getpid and Getppid, types.go's Getpagesize, and sys.go, sys_linux.go,
 * sys_bsd.go and sys_windows.go, from the same release.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/path/filepath.h"
#include "burrow/sync.h"

#include "internal.h"

#include <string.h>

#define OS_LIT(s) str_from_bytes((const Byte *)(s), (Int)(sizeof(s) - 1))

static Str os_proc_copy(Alloc *a, const Byte *p, Int n) {
    if (n == 0)
        return (Str){(const Byte *)"", 0};
    Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (b == NULL)
        return (Str){NULL, 0};
    memcpy(b, p, (size_t)n);
    return str_from_bytes(b, n);
}

/* -------------------------------------------------------------------- Args */

/* Read once, the first time anyone asks, and kept for the life of the
 * process, the way Go's runtime fills os.Args before main. */
static SyncOnce os_args_once;
static Slice os_args_slice;

static void os_args_init(void *env) {
    (void)env;
    os_args_slice = slice_nil(TYPE_STRING);
    int64_t cap = 4096;
    char *buf = NULL;
    int64_t n = -1;
    for (;;) {
        buf = (char *)mem_alloc_nozero(heap_allocator(), (size_t)cap, 1);
        if (buf == NULL)
            return;
        PalErrno pe = PAL_OK;
        n = pal_args(buf, cap, &pe);
        if (n >= 0 || pe != PAL_ERANGE || cap > ((int64_t)1 << 28))
            break;
        mem_free(heap_allocator(), buf, (size_t)cap, 1);
        buf = NULL;
        cap *= 4;
    }
    if (n > 0) {
        Int count = 0;
        for (int64_t i = 0; i < n; i++)
            count += buf[i] == 0;
        Slice s = slice_make(heap_allocator(), TYPE_STRING, 0, count);
        for (int64_t i = 0; i < n;) {
            Int len = (Int)strlen(buf + i);
            Str arg = len == 0 ? (Str){(const Byte *)"", 0}
                               : str_from_bytes((const Byte *)buf + i, len);
            s = slice_append(heap_allocator(), s, &arg, 1);
            i += len + 1;
        }
        os_args_slice = s;
        /* The strings point into buf, which is why it stays. */
        return;
    }
    if (buf != NULL)
        mem_free(heap_allocator(), buf, (size_t)cap, 1);
}

Slice os_args(void) {
    sync_once_do(&os_args_once, BURROW_FN(Func, os_args_init, NULL));
    return os_args_slice;
}

/* ---------------------------------------------------------------------- ids */

Int os_getuid(void) {
    PalIds ids;
    pal_ids(&ids);
    return (Int)ids.uid;
}

Int os_geteuid(void) {
    PalIds ids;
    pal_ids(&ids);
    return (Int)ids.euid;
}

Int os_getgid(void) {
    PalIds ids;
    pal_ids(&ids);
    return (Int)ids.gid;
}

Int os_getegid(void) {
    PalIds ids;
    pal_ids(&ids);
    return (Int)ids.egid;
}

Slice os_getgroups(Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice out = slice_nil(TYPE_INT);
#if defined(BURROW_OS_WINDOWS)
    (void)a;
    BURROW_OUT(err, os_new_syscall_error(error_allocator(), OS_LIT("getgroups"),
                                         burrow__os_errno_value(SYSCALL_EWINDOWS)));
    return out;
#else
    PalErrno pe = PAL_OK;
    int64_t n = pal_getgroups(NULL, 0, &pe);
    if (n < 0) {
        BURROW_OUT(err, os_new_syscall_error(error_allocator(), OS_LIT("getgroups"),
                                             burrow__os_errno(pe)));
        return out;
    }
    if (n == 0)
        return out;
    /* What Go's Getgroups calls a sanity check. */
    if (n > (1 << 20)) {
        BURROW_OUT(err, os_new_syscall_error(error_allocator(), OS_LIT("getgroups"),
                                             burrow__os_errno_value(SYSCALL_EINVAL)));
        return out;
    }
    size_t size = (size_t)n * sizeof(uint32_t);
    uint32_t *gids =
        (uint32_t *)mem_alloc_nozero(heap_allocator(), size, _Alignof(uint32_t));
    if (gids == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return out;
    }
    int64_t got = pal_getgroups(gids, n, &pe);
    if (got < 0) {
        BURROW_OUT(err, os_new_syscall_error(error_allocator(), OS_LIT("getgroups"),
                                             burrow__os_errno(pe)));
    } else {
        out = slice_make(a, TYPE_INT, 0, (Int)got);
        for (int64_t i = 0; i < got; i++) {
            Int g = (Int)gids[i];
            out = slice_append(a, out, &g, 1);
        }
    }
    mem_free(heap_allocator(), gids, size, _Alignof(uint32_t));
    return out;
#endif
}

Int os_getpid(void) {
    return (Int)pal_getpid();
}

Int os_getppid(void) {
    return (Int)pal_getppid();
}

Int os_getpagesize(void) {
    return (Int)pal_page_size();
}

void os_exit(Int code) {
    pal_exit((int32_t)code);
}

/* ----------------------------------------------------------------- Hostname */

Str os_hostname(Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    char buf[1026];
    PalErrno pe = PAL_OK;
    int64_t n = pal_hostname(buf, (int64_t)sizeof buf, &pe);
    if (n < 0) {
        Error e = burrow__os_errno(pe);
#if defined(BURROW_OS_WINDOWS)
        e = os_new_syscall_error(error_allocator(), OS_LIT("ComputerNameEx"), e);
#elif defined(BURROW_OS_LINUX)
        /* Go only fails here when /proc could not be read. */
        e = fs_path_error_new(error_allocator(), OS_LIT("open"),
                              OS_LIT("/proc/sys/kernel/hostname"), e);
#else
        e = os_new_syscall_error(error_allocator(), OS_LIT("sysctl kern.hostname"), e);
#endif
        BURROW_OUT(err, e);
        return (Str){NULL, 0};
    }
    Str s = os_proc_copy(a, (const Byte *)buf, (Int)n);
    if (s.p == NULL)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return s;
}

/* ------------------------------------------------------------- Getwd, Chdir */

/* syscall.Getwd: the system's answer, growing the buffer until it fits or the
 * system says something other than that it does not. */
static Str os_syscall_getwd(Alloc *a, PalErrno *pe) {
    char small[1024];
    char *buf = small;
    int64_t cap = (int64_t)sizeof small;
    Str out = {NULL, 0};
    for (;;) {
        *pe = PAL_OK;
        int64_t n = pal_getcwd(buf, cap, pe);
        if (n >= 0) {
            out = os_proc_copy(a, (const Byte *)buf, (Int)n);
            if (out.p == NULL)
                *pe = PAL_ENOMEM;
            break;
        }
        if (buf != small)
            mem_free(heap_allocator(), buf, (size_t)cap, 1);
        buf = small;
        if (*pe != PAL_ERANGE || cap >= ((int64_t)1 << 16))
            break;
        cap *= 4;
        char *nbuf = (char *)mem_alloc_nozero(heap_allocator(), (size_t)cap, 1);
        if (nbuf == NULL) {
            *pe = PAL_ENOMEM;
            break;
        }
        buf = nbuf;
    }
    if (buf != small)
        mem_free(heap_allocator(), buf, (size_t)cap, 1);
    return out;
}

#if defined(BURROW_OS_WINDOWS)

Str os_getwd(Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    PalErrno pe = PAL_OK;
    Str dir = os_syscall_getwd(a, &pe);
    if (pe != PAL_OK)
        BURROW_OUT(err, os_new_syscall_error(error_allocator(), OS_LIT("getwd"),
                                             burrow__os_errno(pe)));
    return dir;
}

#else

/* getwdCache: the last answer the slow way found, which is worth checking
 * first next time because the slow way is very slow. */
static SyncMutex os_getwd_mu;
static Str os_getwd_cache;

static void os_getwd_cache_set(Str dir) {
    Str keep = os_proc_copy(heap_allocator(), dir.p, dir.len);
    sync_mutex_lock(&os_getwd_mu);
    Str old = os_getwd_cache;
    os_getwd_cache = keep;
    sync_mutex_unlock(&os_getwd_mu);
    if (old.len > 0)
        mem_free(heap_allocator(), (void *)(uintptr_t)old.p, (size_t)old.len, 1);
}

static Str os_getwd_cache_get(Alloc *a) {
    sync_mutex_lock(&os_getwd_mu);
    Str s = os_proc_copy(a, os_getwd_cache.p, os_getwd_cache.len);
    sync_mutex_unlock(&os_getwd_mu);
    return s;
}

/* Whether name is the same file as dot, with any error ignored. */
static bool os_same_as(Alloc *a, Str name, OsFileInfo dot, bool follow) {
    Error e = BURROW_NO_ERROR;
    OsFileInfo d = follow ? os_stat(a, name, &e) : os_lstat(a, name, &e);
    return BURROW_OK(e) && os_same_file(dot, d);
}

/* The slow way: up through "..", finding each directory's name in its parent
 * by comparing what is there against where we came from. */
static Str os_getwd_walk(Alloc *a, Alloc *sa, OsFileInfo dot, OsFileInfo root,
                         Error *err) {
    Str dir = (Str){NULL, 0};
    Str parent = OS_LIT("..");
    for (;;) {
        if (parent.len >= 1024) {
            BURROW_OUT(err, os_new_syscall_error(
                                error_allocator(), OS_LIT("getwd"),
                                burrow__os_errno_value(SYSCALL_ENAMETOOLONG)));
            return (Str){NULL, 0};
        }
        Error e = BURROW_NO_ERROR;
        OsFile *fd = os_open(sa, parent, &e);
        if (fd == NULL) {
            BURROW_OUT(err, e);
            return (Str){NULL, 0};
        }
        bool found = false;
        while (!found) {
            Slice names = os_file_readdirnames(fd, sa, 100, &e);
            if (BURROW_FAILED(e)) {
                os_file_free(fd);
                BURROW_OUT(err, os_new_syscall_error(
                                    error_allocator(), OS_LIT("getwd"),
                                    burrow__os_errno_value(SYSCALL_ENAMETOOLONG)));
                return (Str){NULL, 0};
            }
            for (Int i = 0; i < names.len; i++) {
                Str name = *(const Str *)slice_at(names, i);
                Str path = burrow__os_cat3(sa, parent, OS_LIT("/"), name);
                if (os_same_as(sa, path, dot, false)) {
                    dir = burrow__os_cat3(sa, OS_LIT("/"), name, dir);
                    found = true;
                    break;
                }
            }
        }
        OsFileInfo pd = os_file_stat(fd, sa, &e);
        os_file_free(fd);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return (Str){NULL, 0};
        }
        if (os_same_file(pd, root))
            break;
        dot = pd;
        parent = burrow__os_cat3(sa, OS_LIT("../"), parent, (Str){NULL, 0});
    }
    os_getwd_cache_set(dir);
    Str out = os_proc_copy(a, dir.p, dir.len);
    if (out.p == NULL)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return out;
}

static Str os_getwd_in(Alloc *a, Alloc *sa, Error *err) {
    Error e = BURROW_NO_ERROR;

    /* $PWD when it names this directory, because it keeps the symbolic links
     * the user went through and the system's answer does not. */
    OsFileInfo dot = {NULL, NULL};
    Str dir = os_getenv(sa, OS_LIT("PWD"));
    if (dir.len > 0 && dir.p[0] == '/') {
        dot = os_stat(sa, OS_LIT("."), &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return (Str){NULL, 0};
        }
        if (os_same_as(sa, dir, dot, true))
            return os_proc_copy(a, dir.p, dir.len);
    }

    PalErrno pe = PAL_OK;
    dir = os_syscall_getwd(a, &pe);
    if (pe != PAL_ENAMETOOLONG && pe != PAL_EINVAL && pe != PAL_ERANGE &&
        pe != PAL_ENOMEM) {
        if (pe != PAL_OK)
            BURROW_OUT(err, os_new_syscall_error(error_allocator(), OS_LIT("getwd"),
                                                 burrow__os_errno(pe)));
        return dir;
    }

    if (dot.vt == NULL) {
        dot = os_stat(sa, OS_LIT("."), &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return (Str){NULL, 0};
        }
    }
    dir = os_getwd_cache_get(sa);
    if (dir.len > 0 && os_same_as(sa, dir, dot, true))
        return os_proc_copy(a, dir.p, dir.len);
    OsFileInfo root = os_stat(sa, OS_LIT("/"), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return (Str){NULL, 0};
    }
    if (os_same_file(root, dot))
        return os_proc_copy(a, (const Byte *)"/", 1);
    return os_getwd_walk(a, sa, dot, root, err);
}

Str os_getwd(Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str out = os_getwd_in(a, arena_allocator(&ar), err);
    arena_free(&ar);
    return out;
}

#endif

Error os_chdir(Str dir) {
    Error e = BURROW_NO_ERROR;
    OsCPath c;
    if (burrow__os_cpath(&c, dir, &e)) {
        PalErrno pe = PAL_OK;
        if (!pal_chdir(c.p, &pe))
            e = burrow__os_errno(pe);
        burrow__os_cpath_free(&c);
    }
    if (BURROW_FAILED(e))
        return fs_path_error_new(error_allocator(), OS_LIT("chdir"), dir, e);
    return BURROW_NO_ERROR;
}

/* -------------------------------------------------------- user directories */

static Str os_env_dir(Alloc *a, Str name, Str suffix, Str missing, Error *err) {
    Str dir = os_getenv(a, name);
    if (dir.len == 0) {
        BURROW_OUT(err, errors_new(error_allocator(), missing));
        return (Str){NULL, 0};
    }
    if (suffix.len == 0)
        return dir;
    Str out = burrow__os_cat3(a, dir, suffix, (Str){NULL, 0});
    if (out.p == NULL)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return out;
}

#if !defined(BURROW_OS_WINDOWS) && !defined(BURROW_OS_DARWIN)
/* The XDG variable when it is set and absolute, $HOME and fallback when it is
 * not set, and an error when it is relative. */
static Str os_xdg_dir(Alloc *a, Str var, Str fallback, Str missing, Str relative,
                      Error *err) {
    Str dir = os_getenv(a, var);
    if (dir.len == 0)
        return os_env_dir(a, OS_LIT("HOME"), fallback, missing, err);
    if (!filepath_is_abs(dir)) {
        BURROW_OUT(err, errors_new(error_allocator(), relative));
        return (Str){NULL, 0};
    }
    return dir;
}
#endif

Str os_user_cache_dir(Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
#if defined(BURROW_OS_WINDOWS)
    return os_env_dir(a, OS_LIT("LocalAppData"), (Str){NULL, 0},
                      OS_LIT("%LocalAppData% is not defined"), err);
#elif defined(BURROW_OS_DARWIN)
    return os_env_dir(a, OS_LIT("HOME"), OS_LIT("/Library/Caches"),
                      OS_LIT("$HOME is not defined"), err);
#else
    return os_xdg_dir(a, OS_LIT("XDG_CACHE_HOME"), OS_LIT("/.cache"),
                      OS_LIT("neither $XDG_CACHE_HOME nor $HOME are defined"),
                      OS_LIT("path in $XDG_CACHE_HOME is relative"), err);
#endif
}

Str os_user_config_dir(Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
#if defined(BURROW_OS_WINDOWS)
    return os_env_dir(a, OS_LIT("AppData"), (Str){NULL, 0},
                      OS_LIT("%AppData% is not defined"), err);
#elif defined(BURROW_OS_DARWIN)
    return os_env_dir(a, OS_LIT("HOME"), OS_LIT("/Library/Application Support"),
                      OS_LIT("$HOME is not defined"), err);
#else
    return os_xdg_dir(a, OS_LIT("XDG_CONFIG_HOME"), OS_LIT("/.config"),
                      OS_LIT("neither $XDG_CONFIG_HOME nor $HOME are defined"),
                      OS_LIT("path in $XDG_CONFIG_HOME is relative"), err);
#endif
}

Str os_user_home_dir(Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
#if defined(BURROW_OS_WINDOWS)
    return os_env_dir(a, OS_LIT("USERPROFILE"), (Str){NULL, 0},
                      OS_LIT("%userprofile% is not defined"), err);
#else
    return os_env_dir(a, OS_LIT("HOME"), (Str){NULL, 0}, OS_LIT("$HOME is not defined"),
                      err);
#endif
}

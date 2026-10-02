/* os.Root.
 *
 * Derived from Go's src/os/root.go, root_openat.go, root_unix.go and
 * removeall_at.go, and on Windows from root_noopenat.go and root_js.go.
 * Go source: go1.27.1.
 *
 * Off Windows a root holds a descriptor and every name is walked from it one
 * component at a time with the PAL's at calls, the same as Go on Unix. On
 * Windows a root is a name, and each call checks the name it is given with
 * lstat and readlink before handing the joined name to the ordinary call, the
 * way Go does on js and plan9. Go's Windows version opens relative to a handle
 * with NtCreateFile, and that is still to come here.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/func.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/path/filepath.h"
#include "burrow/slices.h"
#include "burrow/strings.h"
#include "burrow/sync.h"

#include "internal.h"

#include <string.h>

#define OS_LIT(s) str_from_bytes((const Byte *)(s), (Int)(sizeof(s) - 1))

#if defined(BURROW_OS_WINDOWS)
#define OS_ROOT_BY_NAME 1
#else
#define OS_ROOT_BY_NAME 0
#endif

/* Go makes most of these afresh with errors.New where they are used. One
 * sentinel each says the same thing. errPathEscapes is a single value in Go
 * too. */
BURROW_SENTINEL_ERROR(burrow__os_err_path_escapes, "path escapes from parent");
BURROW_SENTINEL_ERROR(burrow__os_err_root_not_dir, "not a directory");
BURROW_SENTINEL_ERROR(burrow__os_err_root_mode, "unsupported file mode");
BURROW_SENTINEL_ERROR(burrow__os_err_root_empty, "empty path");
#if OS_ROOT_BY_NAME
BURROW_SENTINEL_ERROR(burrow__os_err_root_hard_link,
                      "cannot create a hard link to a symlink");
BURROW_SENTINEL_ERROR(burrow__os_err_root_symlinks, "too many symlinks");
#endif

/* rootMaxSymlinks: how many links one name may go through. */
#define OS_ROOT_MAX_SYMLINKS 8

/* root, with the name's bytes after the struct. fd is not used on Windows. */
struct OsRoot {
    SyncMutex mu;
    int64_t fd;
    Int refs;
    bool closed;
    Alloc *a;
    Str name;
};

static Error os_root_path_error(Str op, Str path, Error e) {
    return fs_path_error_new(error_allocator(), op, path, e);
}

static bool os_root_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* err == syscall.EXXX, which in Go looks at err itself and not inside it. */
static bool os_root_is_errno(Error e, SyscallErrno n) {
    return e.vt != NULL && e.vt->self_type == TYPE_SYSCALL_ERRNO &&
           *(const SyscallErrno *)e.data == n;
}

/* underlyingError: the error inside a PathError, LinkError or SyscallError. */
static Error os_root_underlying(Error e) {
    if (e.vt == NULL || e.vt->self_type == NULL)
        return e;
    const Type *t = e.vt->self_type;
    if (t == TYPE_FS_PATH_ERROR)
        return ((const FsPathError *)e.data)->err;
    if (t == TYPE_OS_LINK_ERROR)
        return ((const OsLinkError *)e.data)->err;
    if (t == TYPE_OS_SYSCALL_ERROR)
        return ((const OsSyscallError *)e.data)->err;
    return e;
}

static bool os_root_is_dot(Str s) {
    return s.len == 1 && s.p[0] == '.';
}

static bool os_root_is_dotdot(Str s) {
    return s.len == 2 && s.p[0] == '.' && s.p[1] == '.';
}

/* joinPath: dir and name with one separator between them, in a. */
static Str os_root_join(Alloc *a, Str dir, Str name) {
    if (dir.len > 0 && os_is_path_separator(dir.p[dir.len - 1]))
        return burrow__os_cat3(a, dir, name, (Str){NULL, 0});
    Byte sep = (Byte)OS_PATH_SEPARATOR;
    return burrow__os_cat3(a, dir, str_from_bytes(&sep, 1), name);
}

/* Gives back a Str os_root_join or burrow__os_cat3 made in the heap. */
static void os_root_str_free(Str s) {
    if (s.p != NULL)
        mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len + 1, 1);
}

/* endsWithDot: "." or anything ending in a separator and a dot. */
static bool os_root_ends_with_dot(Str path) {
    if (os_root_is_dot(path))
        return true;
    return path.len >= 2 && path.p[path.len - 1] == '.' &&
           os_is_path_separator(path.p[path.len - 2]);
}

/* ------------------------------------------------------- splitPathInRoot */

/* The components of a name. Each one ends in a NUL past its length, so it
 * can go to the PAL as it is. They all live in the walk's arena. */
typedef struct OsRootParts {
    Str *p;
    Int len;
    Int cap;
} OsRootParts;

static bool os_root_push(Alloc *t, OsRootParts *ps, Str s) {
    if (ps->len == ps->cap) {
        Int ncap = ps->cap == 0 ? 8 : ps->cap * 2;
        Str *np = (Str *)mem_alloc_nozero(t, (size_t)ncap * sizeof(Str), _Alignof(Str));
        if (np == NULL)
            return false;
        if (ps->len > 0)
            memcpy(np, ps->p, (size_t)ps->len * sizeof(Str));
        ps->p = np;
        ps->cap = ncap;
    }
    ps->p[ps->len++] = s;
    return true;
}

/* A copy of s with a NUL after it. */
static Str os_root_dup(Alloc *t, Str s) {
    Byte *b = (Byte *)mem_alloc_nozero(t, (size_t)s.len + 1, 1);
    if (b == NULL)
        return (Str){NULL, 0};
    if (s.len > 0)
        memcpy(b, s.p, (size_t)s.len);
    b[s.len] = 0;
    return str_from_bytes(b, s.len);
}

#if !OS_ROOT_BY_NAME
static const char *os_root_c(Str s) {
    return (const char *)s.p;
}
#endif

/* splitPathInRoot: s cut into its components, with prefix in front and
 * suffix behind. "." components go, except one that is the whole of it, and
 * ".." stays for the walk to deal with. ends_in_slash says s had a separator
 * at the end. */
static Error os_root_split(Alloc *t, Str s, const Str *prefix, Int nprefix,
                           const Str *suffix, Int nsuffix, OsRootParts *out,
                           bool *ends_in_slash) {
    *ends_in_slash = false;
    *out = (OsRootParts){NULL, 0, 0};
    if (s.len == 0)
        return burrow__os_err_root_empty;
    if (os_is_path_separator(s.p[0]))
        return burrow__os_err_path_escapes;
#if OS_ROOT_BY_NAME
    /* rootCleanPath refuses a "?" anywhere, since no Windows name has one. */
    if (memchr(s.p, '?', (size_t)s.len) != NULL)
        return burrow__os_errno_value((SyscallErrno)123); /* ERROR_INVALID_NAME */
#endif
    for (Int k = 0; k < nprefix; k++)
        if (!os_root_push(t, out, prefix[k]))
            return burrow_err_out_of_memory;
    Int i = 0, j = 1;
    for (;;) {
        if (j < s.len && !os_is_path_separator(s.p[j])) {
            j++;
            continue;
        }
        Str part = os_root_dup(t, str_from_bytes(s.p + i, j - i));
        if (part.p == NULL || !os_root_push(t, out, part))
            return burrow_err_out_of_memory;
        Int part_end = j;
        while (j < s.len && os_is_path_separator(s.p[j]))
            j++;
        if (j == s.len) {
            if (s.len - part_end > 0)
                *ends_in_slash = true;
            break;
        }
        if (os_root_is_dot(out->p[out->len - 1]))
            out->len--;
        i = j;
    }
    if (nsuffix > 0 && out->len > 0 && os_root_is_dot(out->p[out->len - 1]))
        out->len--;
    for (Int k = 0; k < nsuffix; k++)
        if (!os_root_push(t, out, suffix[k]))
            return burrow_err_out_of_memory;
#if OS_ROOT_BY_NAME
    /* rootCleanPath ends with filepathlite.IsLocal, which also turns away a
     * drive letter and the reserved names such as NUL and COM1. Done here a
     * component at a time, since ".." is still the walk's to handle. */
    for (Int k = nprefix; k < out->len - nsuffix; k++) {
        Str c = out->p[k];
        if (!os_root_is_dot(c) && !os_root_is_dotdot(c) && !filepath_is_local(c))
            return burrow__os_err_path_escapes;
    }
#endif
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------- the root */

static OsRoot *os_root_new(Alloc *a, int64_t fd, Str name) {
    OsRoot *r =
        (OsRoot *)mem_alloc(a, sizeof(OsRoot) + (size_t)name.len, _Alignof(OsRoot));
    if (r == NULL)
        return NULL;
    Byte *p = (Byte *)(r + 1);
    if (name.len > 0)
        memcpy(p, name.p, (size_t)name.len);
    r->name = str_from_bytes(p, name.len);
    r->fd = fd;
    r->a = a;
    return r;
}

Str os_root_name(const OsRoot *r) {
    return r->name;
}

/* Go's root.incref and decref. The by-name roots on Windows hold no
 * handle, so they only look at closed. */
#if !OS_ROOT_BY_NAME
static bool os_root_incref(OsRoot *r) {
    sync_mutex_lock(&r->mu);
    bool ok = !r->closed;
    if (ok)
        r->refs++;
    sync_mutex_unlock(&r->mu);
    return ok;
}

static void os_root_decref(OsRoot *r) {
    sync_mutex_lock(&r->mu);
    r->refs--;
    bool last = r->closed && r->refs == 0;
    sync_mutex_unlock(&r->mu);
    if (last)
        pal_close(r->fd, NULL);
}
#endif

Error os_root_close(OsRoot *r) {
    sync_mutex_lock(&r->mu);
    bool now = !r->closed && r->refs == 0;
    r->closed = true;
    sync_mutex_unlock(&r->mu);
#if !OS_ROOT_BY_NAME
    if (now)
        pal_close(r->fd, NULL);
#else
    (void)now;
#endif
    return BURROW_NO_ERROR;
}

void os_root_free(OsRoot *r) {
    if (r == NULL)
        return;
    os_root_close(r);
    mem_free(r->a, r, sizeof(OsRoot) + (size_t)r->name.len, _Alignof(OsRoot));
}

OsFile *os_open_in_root(Alloc *a, Str dir, Str name, Error *err) {
    OsRoot *r = os_open_root(heap_allocator(), dir, err);
    if (r == NULL)
        return NULL;
    OsFile *f = os_root_open(r, a, name, err);
    os_root_free(r);
    return f;
}

OsFile *os_root_open(OsRoot *r, Alloc *a, Str name, Error *err) {
    return os_root_open_file(r, a, name, OS_O_RDONLY, 0, err);
}

OsFile *os_root_create(OsRoot *r, Alloc *a, Str name, Error *err) {
    return os_root_open_file(r, a, name, OS_O_RDWR | OS_O_CREATE | OS_O_TRUNC, 0666,
                             err);
}

static OsFile *os_root_open_file_nolog(OsRoot *r, Alloc *a, Str name, Int flag,
                                       OsFileMode perm, Error *err);

OsFile *os_root_open_file(OsRoot *r, Alloc *a, Str name, Int flag, OsFileMode perm,
                          Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if ((perm & 0777) != perm) {
        BURROW_OUT(
            err, os_root_path_error(OS_LIT("openat"), name, burrow__os_err_root_mode));
        return NULL;
    }
    OsFile *f = os_root_open_file_nolog(r, a, name, flag, perm, err);
    if (f != NULL)
        f->append_mode = (flag & OS_O_APPEND) != 0;
    return f;
}

static Error os_root_mkdir_impl(OsRoot *r, Str name, OsFileMode perm);
static Error os_root_mkdir_all_impl(OsRoot *r, Str name, OsFileMode perm);

Error os_root_mkdir(OsRoot *r, Str name, OsFileMode perm) {
    if ((perm & 0777) != perm)
        return os_root_path_error(OS_LIT("mkdirat"), name, burrow__os_err_root_mode);
    return os_root_mkdir_impl(r, name, perm);
}

Error os_root_mkdir_all(OsRoot *r, Str name, OsFileMode perm) {
    if ((perm & 0777) != perm)
        return os_root_path_error(OS_LIT("mkdirat"), name, burrow__os_err_root_mode);
    return os_root_mkdir_all_impl(r, name, perm);
}

Slice os_root_read_file(OsRoot *r, Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_root_open(r, heap_allocator(), name, &e);
    if (f == NULL) {
        BURROW_OUT(err, e);
        return (Slice){NULL, 0, 0, TYPE_BYTE};
    }
    Slice data = burrow__os_read_all(f, a, &e);
    os_file_free(f);
    BURROW_OUT(err, e);
    return data;
}

Error os_root_write_file(OsRoot *r, Str name, Slice data, OsFileMode perm) {
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_root_open_file(r, heap_allocator(), name,
                                  OS_O_WRONLY | OS_O_CREATE | OS_O_TRUNC, perm, &e);
    if (f == NULL)
        return e;
    os_file_write(f, data, &e);
    Error ce = os_file_close(f);
    os_file_free(f);
    if (BURROW_OK(e))
        e = ce;
    return e;
}

#if !OS_ROOT_BY_NAME

/* ================================================================ openat */

BURROW_SENTINEL_ERROR(burrow__os_err_root_symlink, "errSymlink is not user-visible");

/* One walk through a name. link is the target of the link the last step ran
 * into, when a step returns burrow__os_err_root_symlink. */
typedef struct OsRootWalk {
    Alloc *t;
    Str link;
} OsRootWalk;

/* checkSymlink: if name in parent is a link, its target in w->link and the
 * sentinel back, and otherwise orig. A walk of NULL only asks whether it is a
 * link, which removeAllFrom is happy with. */
static Error os_root_check_symlink(OsRootWalk *w, int64_t parent, Str name,
                                   Error orig) {
    if (w == NULL) {
        char b[1];
        PalErrno pe = PAL_OK;
        int64_t n = pal_readlinkat(parent, os_root_c(name), b, 1, &pe);
        if (n < 0 && pe != PAL_ERANGE)
            return orig;
        return burrow__os_err_root_symlink;
    }
    for (Int len = 128;; len *= 2) {
        char *b = (char *)mem_alloc_nozero(w->t, (size_t)len, 1);
        if (b == NULL)
            return burrow_err_out_of_memory;
        PalErrno pe = PAL_OK;
        int64_t n = pal_readlinkat(parent, os_root_c(name), b, len, &pe);
        if (n < 0 && pe == PAL_ERANGE)
            continue;
        if (n < 0)
            return orig;
        w->link = str_from_bytes((const Byte *)b, (Int)n);
        return burrow__os_err_root_symlink;
    }
}

/* isNoFollowErr: what open says when O_NOFOLLOW stopped it at a link. */
static bool os_root_no_follow(PalErrno pe) {
    return pe == PAL_ELOOP || pe == PAL_EMLINK;
}

/* rootOpenDir. */
static Error os_root_open_dir(OsRootWalk *w, int64_t parent, Str name, int64_t *fd) {
    PalErrno pe = PAL_OK;
    *fd = pal_openat(parent, os_root_c(name),
                     PAL_O_RDONLY | PAL_O_NOFOLLOW | PAL_O_DIRECTORY, 0, &pe);
    if (*fd != PAL_INVALID_HANDLE)
        return BURROW_NO_ERROR;
    Error err = burrow__os_errno(pe);
    if (os_root_no_follow(pe) || pe == PAL_ENOTDIR)
        err = os_root_check_symlink(w, parent, name, err);
    else if (pe == PAL_ENOTSUP)
        err = burrow__os_errno_value(SYSCALL_ENOTDIR);
    return err;
}

/* The two kinds of step: one into a directory on the way, and the last. */
typedef Error (*OsRootDirFn)(OsRootWalk *w, void *env, int64_t parent, Str name,
                             int64_t *fd);
typedef Error (*OsRootLastFn)(OsRootWalk *w, void *env, int64_t parent, Str name,
                              bool ends_in_slash);

enum {
    /* The function handles a trailing slash itself. */
    OS_ROOT_NO_HANDLE_TERMINAL_SLASH = 1 << 0,
    /* The last component may be missing, since it is about to be made. */
    OS_ROOT_CREATING_DIRECTORY = 1 << 1
};

static void os_root_close_dir(int64_t *dirfd, int64_t rootfd) {
    if (*dirfd != rootfd)
        pal_close(*dirfd, NULL);
    *dirfd = rootfd;
}

/* doInRoot's loop, with the root's reference already held. */
static Error os_root_walk(OsRoot *r, OsRootWalk *w, Str name, unsigned flags,
                          OsRootDirFn open_dir, OsRootLastFn f, void *env) {
    enum { MAX_STEPS = 255, MAX_RESTARTS = 8 };
    static const Byte dot[] = ".";
    OsRootParts parts;
    bool ends_in_slash;
    Error err = os_root_split(w->t, name, NULL, 0, NULL, 0, &parts, &ends_in_slash);
    if (BURROW_FAILED(err))
        return err;

    int64_t rootfd = r->fd;
    int64_t dirfd = rootfd;
    Int i = 0, steps = 0, restarts = 0, symlinks = 0;
    for (;;) {
        steps++;
        if (steps > MAX_STEPS && restarts > MAX_RESTARTS) {
            err = burrow__os_errno_value(SYSCALL_ENAMETOOLONG);
            break;
        }

        if (os_root_is_dotdot(parts.p[i])) {
            /* Resolve one or more ".." by dropping them and the components
             * before them, and start again from the root. */
            restarts++;
            Int end = i + 1;
            while (end < parts.len && os_root_is_dotdot(parts.p[end]))
                end++;
            Int count = end - i;
            if (count > i) {
                err = burrow__os_err_path_escapes;
                break;
            }
            memmove(parts.p + i - count, parts.p + end,
                    (size_t)(parts.len - end) * sizeof(Str));
            parts.len -= 2 * count;
            if (parts.len == 0) {
                parts.p[0] = str_from_bytes(dot, 1);
                parts.len = 1;
            }
            i = 0;
            os_root_close_dir(&dirfd, rootfd);
            continue;
        }

        if (i == parts.len - 1) {
            /* The last component, which may end in a slash. */
            err = BURROW_NO_ERROR;
            if (ends_in_slash && (flags & OS_ROOT_NO_HANDLE_TERMINAL_SLASH) == 0) {
                PalStat st;
                PalErrno pe = PAL_OK;
                if (!pal_lstatat(dirfd, os_root_c(parts.p[i]), &st, &pe)) {
                    err = burrow__os_errno(pe);
                    if (os_is_not_exist(err) &&
                        (flags & OS_ROOT_CREATING_DIRECTORY) != 0)
                        err = BURROW_NO_ERROR;
                    else
                        break;
                } else if ((st.mode & PAL_S_IFMT) == PAL_S_IFDIR) {
                    /* fine as it is */
                } else if ((st.mode & PAL_S_IFMT) == PAL_S_IFLNK) {
                    err = os_root_check_symlink(
                        w, dirfd, parts.p[i], burrow__os_errno_value(SYSCALL_ENOTDIR));
                } else {
                    err = burrow__os_errno_value(SYSCALL_ENOTDIR);
                    break;
                }
            }
            if (BURROW_OK(err)) {
                err = f(w, env, dirfd, parts.p[i], ends_in_slash);
                if (BURROW_OK(err))
                    break;
            }
        } else {
            /* A directory on the way. */
            int64_t fd = PAL_INVALID_HANDLE;
            err = open_dir != NULL ? open_dir(w, env, dirfd, parts.p[i], &fd)
                                   : os_root_open_dir(w, dirfd, parts.p[i], &fd);
            if (BURROW_OK(err)) {
                os_root_close_dir(&dirfd, rootfd);
                dirfd = fd;
            }
        }

        if (BURROW_OK(err)) {
            i++;
            continue;
        }
        if (os_root_same(err, burrow__os_err_root_symlink)) {
            symlinks++;
            if (symlinks > OS_ROOT_MAX_SYMLINKS) {
                err = burrow__os_errno_value(SYSCALL_ELOOP);
                break;
            }
            bool last_part = i == parts.len - 1;
            OsRootParts np;
            bool new_ends_in_slash;
            err = os_root_split(w->t, w->link, parts.p, i, parts.p + i + 1,
                                parts.len - i - 1, &np, &new_ends_in_slash);
            if (BURROW_FAILED(err))
                break;
            if (last_part && new_ends_in_slash)
                ends_in_slash = true;
            /* If the components before this one changed, which "." in the
             * target can do, start again from the root. */
            bool same = np.len >= i;
            for (Int k = 0; same && k < i; k++)
                same = str_eq(parts.p[k], np.p[k]);
            if (!same) {
                i = 0;
                os_root_close_dir(&dirfd, rootfd);
            }
            parts = np;
            continue;
        }
        if (err.vt != NULL && err.vt->self_type == TYPE_FS_PATH_ERROR) {
            /* The step's PathError gets the path as far as this step. */
            const FsPathError *pe = (const FsPathError *)err.data;
            Str path = parts.p[0];
            Byte sep = (Byte)OS_PATH_SEPARATOR;
            for (Int k = 1; k <= i && path.p != NULL; k++)
                path = burrow__os_cat3(w->t, path, str_from_bytes(&sep, 1), parts.p[k]);
            err = path.p == NULL ? burrow_err_out_of_memory
                                 : os_root_path_error(pe->op, path, pe->err);
        }
        break;
    }
    os_root_close_dir(&dirfd, rootfd);
    return err;
}

/* doInRoot: f on the last component of name, with parent open on the
 * directory that holds it, after open_dir, or rootOpenDir when it is NULL,
 * has walked there. */
static Error os_root_do(OsRoot *r, Str name, unsigned flags, OsRootDirFn open_dir,
                        OsRootLastFn f, void *env) {
    if (!os_root_incref(r))
        return fs_err_closed;
    Arena ar;
    arena_init(&ar, NULL, 0);
    OsRootWalk w = {arena_allocator(&ar), {NULL, 0}};
    Error err = os_root_walk(r, &w, name, flags, open_dir, f, env);
    arena_free(&ar);
    os_root_decref(r);
    return err;
}

/* lstatat, for the type of name in parent. */
static Error os_root_mode_at(int64_t parent, Str name, uint32_t *mode) {
    PalStat st;
    PalErrno pe = PAL_OK;
    if (!pal_lstatat(parent, os_root_c(name), &st, &pe))
        return burrow__os_errno(pe);
    *mode = st.mode & PAL_S_IFMT;
    return BURROW_NO_ERROR;
}

static uint32_t os_root_syscall_mode(OsFileMode m) {
    uint32_t o = m & FS_MODE_PERM;
    if (m & FS_MODE_SETUID)
        o |= PAL_S_ISUID;
    if (m & FS_MODE_SETGID)
        o |= PAL_S_ISGID;
    if (m & FS_MODE_STICKY)
        o |= PAL_S_ISVTX;
    return o;
}

/* An OsFile for fd, named for the root and name. Closes fd when it cannot. */
static OsFile *os_root_file(OsRoot *r, Alloc *a, int64_t fd, Str name, Error *err) {
    Str full = os_root_join(heap_allocator(), r->name, name);
    OsFile *f = full.p == NULL ? NULL : os_new_file(a, (Uintptr)fd, full);
    os_root_str_free(full);
    if (f == NULL) {
        pal_close(fd, NULL);
        BURROW_OUT(err, burrow_err_out_of_memory);
    }
    return f;
}

/* ---------------------------------------------------------------- open */

static OsRoot *os_root_from_fd(Alloc *a, int64_t fd, Str name, Error *err) {
    PalStat st;
    if (pal_fstat(fd, &st, NULL) && (st.mode & PAL_S_IFMT) != PAL_S_IFDIR) {
        pal_close(fd, NULL);
        BURROW_OUT(
            err, os_root_path_error(OS_LIT("open"), name, burrow__os_err_root_not_dir));
        return NULL;
    }
    OsRoot *r = os_root_new(a, fd, name);
    if (r == NULL) {
        pal_close(fd, NULL);
        BURROW_OUT(err, burrow_err_out_of_memory);
    }
    return r;
}

OsRoot *os_open_root(Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Error e = BURROW_NO_ERROR;
    OsCPath c;
    if (!burrow__os_cpath(&c, name, &e)) {
        BURROW_OUT(err, os_root_path_error(OS_LIT("open"), name, e));
        return NULL;
    }
    PalErrno pe = PAL_OK;
    int64_t fd = pal_open(c.p, PAL_O_RDONLY, 0, &pe);
    if (fd == PAL_INVALID_HANDLE)
        e = burrow__os_errno(pe);
    burrow__os_cpath_free(&c);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, os_root_path_error(OS_LIT("open"), name, e));
        return NULL;
    }
    return os_root_from_fd(a, fd, name, err);
}

typedef struct OsRootOpenEnv {
    uint32_t flag;
    uint32_t perm;
    int64_t fd;
} OsRootOpenEnv;

static Error os_root_open_root_fn(OsRootWalk *w, void *env, int64_t parent, Str name,
                                  bool ends_in_slash) {
    (void)ends_in_slash;
    OsRootOpenEnv *o = (OsRootOpenEnv *)env;
    PalErrno pe = PAL_OK;
    o->fd = pal_openat(parent, os_root_c(name), PAL_O_RDONLY | PAL_O_NOFOLLOW, 0, &pe);
    if (o->fd != PAL_INVALID_HANDLE)
        return BURROW_NO_ERROR;
    Error err = burrow__os_errno(pe);
    if (os_root_no_follow(pe))
        err = os_root_check_symlink(w, parent, name, err);
    return err;
}

OsRoot *os_root_open_root(OsRoot *r, Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    OsRootOpenEnv o = {0, 0, PAL_INVALID_HANDLE};
    Error e = os_root_do(r, name, 0, NULL, os_root_open_root_fn, &o);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, os_root_path_error(OS_LIT("openat"), name, e));
        return NULL;
    }
    Str full = os_root_join(heap_allocator(), r->name, name);
    if (full.p == NULL) {
        pal_close(o.fd, NULL);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    OsRoot *nr = os_root_from_fd(a, o.fd, full, err);
    os_root_str_free(full);
    return nr;
}

static Error os_root_open_file_fn(OsRootWalk *w, void *env, int64_t parent, Str name,
                                  bool ends_in_slash) {
    (void)ends_in_slash;
    OsRootOpenEnv *o = (OsRootOpenEnv *)env;
    PalErrno pe = PAL_OK;
    o->fd = pal_openat(parent, os_root_c(name), o->flag | PAL_O_NOFOLLOW, o->perm, &pe);
    if (o->fd != PAL_INVALID_HANDLE)
        return BURROW_NO_ERROR;
    Error err = burrow__os_errno(pe);
    bool create_excl =
        (o->flag & (PAL_O_CREATE | PAL_O_EXCL)) == (PAL_O_CREATE | PAL_O_EXCL);
    /* With O_CREATE|O_EXCL a link is never followed, and the name being taken
     * by one is the same as it being taken by anything else. */
    if (!create_excl && (os_root_no_follow(pe) || pe == PAL_ENOTDIR))
        err = os_root_check_symlink(w, parent, name, err);
    if (create_excl && pe == PAL_ELOOP)
        err = burrow__os_errno_value(SYSCALL_EEXIST);
    return err;
}

static OsFile *os_root_open_file_nolog(OsRoot *r, Alloc *a, Str name, Int flag,
                                       OsFileMode perm, Error *err) {
    OsRootOpenEnv o = {(uint32_t)flag, (uint32_t)perm, PAL_INVALID_HANDLE};
    Error e = os_root_do(r, name, 0, NULL, os_root_open_file_fn, &o);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, os_root_path_error(OS_LIT("openat"), name, e));
        return NULL;
    }
    return os_root_file(r, a, o.fd, name, err);
}

/* --------------------------------------------------------- stat, readlink */

typedef struct OsRootStatEnv {
    Alloc *a;
    Str name; /* the whole name, whose last element names the FileInfo */
    bool lstat;
    OsFileInfo fi;
} OsRootStatEnv;

static Error os_root_stat_fn(OsRootWalk *w, void *env, int64_t parent, Str n,
                             bool ends_in_slash) {
    (void)ends_in_slash;
    OsRootStatEnv *s = (OsRootStatEnv *)env;
    PalStat st;
    PalErrno pe = PAL_OK;
    if (!pal_lstatat(parent, os_root_c(n), &st, &pe))
        return burrow__os_errno(pe);
    if (!s->lstat && (st.mode & PAL_S_IFMT) == PAL_S_IFLNK)
        return os_root_check_symlink(w, parent, n,
                                     burrow__os_errno_value(SYSCALL_ELOOP));
    Error e = BURROW_NO_ERROR;
    s->fi = burrow__os_file_info(s->a, s->name, &st, &e);
    return e;
}

static OsFileInfo os_root_stat_op(OsRoot *r, Alloc *a, Str name, bool lstat,
                                  Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    OsRootStatEnv s = {a, name, lstat, {NULL, NULL}};
    Error e = os_root_do(r, name, 0, NULL, os_root_stat_fn, &s);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, os_root_path_error(OS_LIT("statat"), name, e));
        return (OsFileInfo){NULL, NULL};
    }
    return s.fi;
}

typedef struct OsRootLinkEnv {
    Alloc *a;
    Str target;
} OsRootLinkEnv;

/* readlinkat, into a. */
static Error os_root_readlink_fn(OsRootWalk *w, void *env, int64_t parent, Str name,
                                 bool ends_in_slash) {
    (void)w;
    (void)ends_in_slash;
    OsRootLinkEnv *l = (OsRootLinkEnv *)env;
    for (Int len = 128;; len *= 2) {
        char *b = (char *)mem_alloc_nozero(l->a, (size_t)len, 1);
        if (b == NULL)
            return burrow_err_out_of_memory;
        PalErrno pe = PAL_OK;
        int64_t n = pal_readlinkat(parent, os_root_c(name), b, len, &pe);
        if (n >= 0) {
            char *s = (char *)mem_realloc(l->a, b, (size_t)len, (size_t)n, 1);
            l->target = str_from_bytes((const Byte *)(s == NULL ? b : s), (Int)n);
            return BURROW_NO_ERROR;
        }
        Error e = pe == PAL_ERANGE ? BURROW_NO_ERROR : burrow__os_errno(pe);
        mem_free(l->a, b, (size_t)len, 1);
        if (BURROW_FAILED(e))
            return e;
    }
}

Str os_root_readlink(OsRoot *r, Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    OsRootLinkEnv l = {a, {NULL, 0}};
    Error e = os_root_do(r, name, 0, NULL, os_root_readlink_fn, &l);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, os_root_path_error(OS_LIT("readlinkat"), name, e));
        return (Str){NULL, 0};
    }
    return l.target;
}

/* ------------------------------------------------- chmod, chown, chtimes */

typedef enum OsRootAttrOp {
    OS_ROOT_CHMOD,
    OS_ROOT_CHOWN,
    OS_ROOT_LCHOWN,
    OS_ROOT_CHTIMES
} OsRootAttrOp;

typedef struct OsRootAttrEnv {
    OsRootAttrOp op;
    OsFileMode mode;
    Int uid;
    Int gid;
    int64_t atime;
    int64_t mtime;
} OsRootAttrEnv;

static Error os_root_attr_fn(OsRootWalk *w, void *env, int64_t parent, Str name,
                             bool ends_in_slash) {
    (void)ends_in_slash;
    OsRootAttrEnv *t = (OsRootAttrEnv *)env;
    /* afterResolvingSymlink: everything but Lchown follows a link by way of
     * the walk, and acts on the name only when it is not one. */
    if (t->op != OS_ROOT_LCHOWN) {
        Error e = os_root_check_symlink(w, parent, name, BURROW_NO_ERROR);
        if (BURROW_FAILED(e))
            return e;
    }
    PalErrno pe = PAL_OK;
    bool ok = false;
    switch (t->op) {
    case OS_ROOT_CHMOD:
        ok = pal_fchmodat(parent, os_root_c(name), os_root_syscall_mode(t->mode), &pe);
        break;
    case OS_ROOT_CHOWN:
    case OS_ROOT_LCHOWN:
        ok = pal_fchownat(parent, os_root_c(name), (int64_t)t->uid, (int64_t)t->gid,
                          &pe);
        break;
    case OS_ROOT_CHTIMES:
        ok = pal_utimesat(parent, os_root_c(name), t->atime, t->mtime, &pe);
        break;
    default:
        break;
    }
    return ok ? BURROW_NO_ERROR : burrow__os_errno(pe);
}

static Error os_root_attr(OsRoot *r, Str name, Str op, OsRootAttrEnv *t) {
    Error e = os_root_do(r, name, 0, NULL, os_root_attr_fn, t);
    if (BURROW_FAILED(e))
        return os_root_path_error(op, name, e);
    return BURROW_NO_ERROR;
}

Error os_root_chmod(OsRoot *r, Str name, OsFileMode mode) {
    OsRootAttrEnv t = {OS_ROOT_CHMOD, mode, 0, 0, 0, 0};
    return os_root_attr(r, name, OS_LIT("chmodat"), &t);
}

Error os_root_chown(OsRoot *r, Str name, Int uid, Int gid) {
    OsRootAttrEnv t = {OS_ROOT_CHOWN, 0, uid, gid, 0, 0};
    return os_root_attr(r, name, OS_LIT("chownat"), &t);
}

Error os_root_lchown(OsRoot *r, Str name, Int uid, Int gid) {
    OsRootAttrEnv t = {OS_ROOT_LCHOWN, 0, uid, gid, 0, 0};
    return os_root_attr(r, name, OS_LIT("lchownat"), &t);
}

Error os_root_chtimes(OsRoot *r, Str name, Time atime, Time mtime) {
    OsRootAttrEnv t = {OS_ROOT_CHTIMES,
                       0,
                       0,
                       0,
                       time_is_zero(atime) ? PAL_UTIME_OMIT : time_unix_nano(atime),
                       time_is_zero(mtime) ? PAL_UTIME_OMIT : time_unix_nano(mtime)};
    return os_root_attr(r, name, OS_LIT("chtimesat"), &t);
}

/* ------------------------------------------------------- mkdir, MkdirAll */

/* Linux and OpenBSD mkdirat take a trailing slash, so Go hands it the name
 * as it is. Elsewhere the walk checks the slash first. */
#if defined(BURROW_OS_LINUX) || defined(__OpenBSD__)
#define OS_ROOT_MKDIR_FLAGS                                                            \
    (OS_ROOT_CREATING_DIRECTORY | OS_ROOT_NO_HANDLE_TERMINAL_SLASH)
#else
#define OS_ROOT_MKDIR_FLAGS OS_ROOT_CREATING_DIRECTORY
#endif

typedef struct OsRootMkdirEnv {
    OsRoot *r;
    Str fullname;
    OsFileMode perm;
} OsRootMkdirEnv;

static Error os_root_mkdirat(int64_t parent, Str name, OsFileMode perm) {
    PalErrno pe = PAL_OK;
    if (pal_mkdirat(parent, os_root_c(name), os_root_syscall_mode(perm), &pe))
        return BURROW_NO_ERROR;
    return burrow__os_errno(pe);
}

static Error os_root_mkdir_fn(OsRootWalk *w, void *env, int64_t parent, Str name,
                              bool ends_in_slash) {
    (void)w;
    (void)ends_in_slash;
    return os_root_mkdirat(parent, name, ((OsRootMkdirEnv *)env)->perm);
}

static Error os_root_mkdir_impl(OsRoot *r, Str name, OsFileMode perm) {
    OsRootMkdirEnv m = {r, name, perm};
    Error e = os_root_do(r, name, OS_ROOT_MKDIR_FLAGS, NULL, os_root_mkdir_fn, &m);
    if (BURROW_FAILED(e))
        return os_root_path_error(OS_LIT("mkdirat"), name, e);
    return BURROW_NO_ERROR;
}

/* MkdirAll's openDirFunc: open the directory, and make it when it is not
 * there. Errors are PathErrors for the walk to put the path in. */
static Error os_root_mkdir_all_dir(OsRootWalk *w, void *env, int64_t parent, Str name,
                                   int64_t *fd) {
    OsRootMkdirEnv *m = (OsRootMkdirEnv *)env;
    for (int try_ = 0;; try_++) {
        Error err = os_root_open_dir(w, parent, name, fd);
        if (BURROW_OK(err) || os_root_same(err, burrow__os_err_root_symlink))
            return err;
        if (try_ > 0 || !os_is_not_exist(err))
            return os_root_path_error(OS_LIT("openat"), (Str){NULL, 0}, err);
        Error me = os_root_mkdirat(parent, name, m->perm);
        if (BURROW_FAILED(me) && !os_root_is_errno(me, SYSCALL_EEXIST))
            return os_root_path_error(OS_LIT("mkdirat"), (Str){NULL, 0}, me);
    }
}

static Error os_root_mkdir_all_last(OsRootWalk *w, void *env, int64_t parent, Str name,
                                    bool ends_in_slash) {
    (void)w;
    (void)ends_in_slash;
    OsRootMkdirEnv *m = (OsRootMkdirEnv *)env;
    Error err = os_root_mkdirat(parent, name, m->perm);
    if (os_root_is_errno(err, SYSCALL_EEXIST)) {
        uint32_t mode = 0;
        if (BURROW_OK(os_root_mode_at(parent, name, &mode))) {
            if (mode == PAL_S_IFDIR) {
                err = BURROW_NO_ERROR;
            } else if (mode == PAL_S_IFLNK) {
                /* A link that leads to a directory is fine, which only
                 * following it in the root can tell. */
                Arena ar;
                arena_init(&ar, NULL, 0);
                Error e = BURROW_NO_ERROR;
                OsFileInfo fi =
                    os_root_stat(m->r, arena_allocator(&ar), m->fullname, &e);
                if (BURROW_OK(e) && fi.vt->is_dir(fi.data))
                    err = BURROW_NO_ERROR;
                else if (BURROW_OK(e))
                    err = burrow__os_errno_value(SYSCALL_ENOTDIR);
                else if (!os_is_not_exist(e))
                    err = e;
                arena_free(&ar);
            }
        }
    }
    if (BURROW_OK(err) || os_root_same(err, burrow__os_err_root_symlink))
        return err;
    return os_root_path_error(OS_LIT("mkdirat"), (Str){NULL, 0}, err);
}

static Error os_root_mkdir_all_impl(OsRoot *r, Str name, OsFileMode perm) {
    OsRootMkdirEnv m = {r, name, perm};
    Error e = os_root_do(r, name, OS_ROOT_MKDIR_FLAGS, os_root_mkdir_all_dir,
                         os_root_mkdir_all_last, &m);
    if (BURROW_FAILED(e) && (e.vt == NULL || e.vt->self_type != TYPE_FS_PATH_ERROR))
        e = os_root_path_error(OS_LIT("mkdirat"), name, e);
    return e;
}

/* ------------------------------------------------------ Remove, RemoveAll */

static Error os_root_remove_fn(OsRootWalk *w, void *env, int64_t parent, Str name,
                               bool ends_in_slash) {
    (void)w;
    (void)env;
    (void)ends_in_slash;
    /* removeat: as a file, then as a directory, and the error that makes
     * more sense when both fail. */
    PalErrno pe = PAL_OK;
    if (pal_unlinkat(parent, os_root_c(name), false, &pe))
        return BURROW_NO_ERROR;
    Error e = burrow__os_errno(pe);
    if (pal_unlinkat(parent, os_root_c(name), true, &pe))
        return BURROW_NO_ERROR;
    Error e1 = burrow__os_errno(pe);
    if (!os_root_is_errno(e1, SYSCALL_ENOTDIR))
        return e1;
    return e;
}

Error os_root_remove(OsRoot *r, Str name) {
    Error e = os_root_do(r, name, 0, NULL, os_root_remove_fn, NULL);
    if (BURROW_FAILED(e))
        return os_root_path_error(OS_LIT("removeat"), name, e);
    return BURROW_NO_ERROR;
}

/* A PathError from below base gets base and a separator in front of its
 * path. */
static Error os_root_prefix_path(Str base, Error e) {
    if (e.vt == NULL || e.vt->self_type != TYPE_FS_PATH_ERROR)
        return e;
    const FsPathError *pe = (const FsPathError *)e.data;
    Byte sep = (Byte)OS_PATH_SEPARATOR;
    Str path =
        burrow__os_cat3(heap_allocator(), base, str_from_bytes(&sep, 1), pe->path);
    if (path.p == NULL)
        return burrow_err_out_of_memory;
    Error n = os_root_path_error(pe->op, path, pe->err);
    os_root_str_free(path);
    return n;
}

/* removeAllFrom: base in parent, and everything under it. */
static Error os_root_remove_all_from(int64_t parent, Str base) {
    OsCPath c;
    Error ce = BURROW_NO_ERROR;
    if (!burrow__os_cpath(&c, base, &ce))
        return os_root_path_error(OS_LIT("unlinkat"), base, ce);

    /* The simple case: a file, or something that is already gone. */
    PalErrno pe = PAL_OK;
    if (pal_unlinkat(parent, c.p, false, &pe)) {
        burrow__os_cpath_free(&c);
        return BURROW_NO_ERROR;
    }
    Error uerr = burrow__os_errno(pe);
    if (os_is_not_exist(uerr)) {
        burrow__os_cpath_free(&c);
        return BURROW_NO_ERROR;
    }
    /* EISDIR is what POSIX says for a directory, and Linux says it. The BSDs
     * and macOS say EPERM, and EACCES may be a directory that is read only. */
    if (pe != PAL_EISDIR && pe != PAL_EPERM && pe != PAL_EACCES) {
        burrow__os_cpath_free(&c);
        return os_root_path_error(OS_LIT("unlinkat"), base, uerr);
    }

    enum { REQ_SIZE = 1024 };
    Error recurse_err = BURROW_NO_ERROR;
    for (;;) {
        int64_t fd = PAL_INVALID_HANDLE;
        Str cbase = str_from_bytes((const Byte *)c.p, base.len);
        Error err = os_root_open_dir(NULL, parent, cbase, &fd);
        if (BURROW_FAILED(err)) {
            if (os_is_not_exist(err)) {
                burrow__os_cpath_free(&c);
                return BURROW_NO_ERROR;
            }
            if (os_root_is_errno(err, SYSCALL_ENOTDIR)) {
                burrow__os_cpath_free(&c);
                return os_root_path_error(OS_LIT("unlinkat"), base, uerr);
            }
            if (os_root_same(err, burrow__os_err_root_symlink))
                err = uerr;
            recurse_err = os_root_path_error(OS_LIT("openfdat"), base, err);
            break;
        }
        OsFile *file = os_new_file(heap_allocator(), (Uintptr)fd, base);
        if (file == NULL) {
            pal_close(fd, NULL);
            burrow__os_cpath_free(&c);
            return burrow_err_out_of_memory;
        }

        Int resp_size = 0;
        for (;;) {
            Int num_err = 0;
            Arena ar;
            arena_init(&ar, NULL, 0);
            Error read_err = BURROW_NO_ERROR;
            Slice names =
                os_file_readdirnames(file, arena_allocator(&ar), REQ_SIZE, &read_err);
            if (BURROW_FAILED(read_err) && !errors_is(read_err, io_eof)) {
                arena_free(&ar);
                os_file_free(file);
                burrow__os_cpath_free(&c);
                if (os_is_not_exist(read_err))
                    return BURROW_NO_ERROR;
                return os_root_path_error(OS_LIT("readdirnames"), base, read_err);
            }
            resp_size = names.len;
            for (Int i = 0; i < names.len; i++) {
                Str name = *(const Str *)slice_at(names, i);
                Error e = os_root_remove_all_from(file->fd, name);
                if (BURROW_FAILED(e)) {
                    e = os_root_prefix_path(base, e);
                    num_err++;
                    if (BURROW_OK(recurse_err))
                        recurse_err = e;
                }
            }
            arena_free(&ar);
            /* If nothing could go, the next batch is new names. Otherwise
             * reading on is fine. */
            if (num_err != REQ_SIZE)
                break;
        }

        /* Reopen rather than read on: removing may have shuffled the
         * directory, issue 20841. */
        os_file_free(file);
        if (resp_size < REQ_SIZE)
            break;
    }

    if (pal_unlinkat(parent, c.p, true, &pe)) {
        burrow__os_cpath_free(&c);
        return BURROW_NO_ERROR;
    }
    Error unlink_err = burrow__os_errno(pe);
    burrow__os_cpath_free(&c);
    if (os_is_not_exist(unlink_err))
        return BURROW_NO_ERROR;
    if (BURROW_FAILED(recurse_err))
        return recurse_err;
    return os_root_path_error(OS_LIT("unlinkat"), base, unlink_err);
}

static Error os_root_remove_all_fn(OsRootWalk *w, void *env, int64_t parent, Str name,
                                   bool ends_in_slash) {
    (void)w;
    (void)env;
    (void)ends_in_slash;
    return os_root_remove_all_from(parent, name);
}

Error os_root_remove_all(OsRoot *r, Str name) {
    while (name.len > 0 && os_is_path_separator(name.p[name.len - 1]))
        name.len--;
    if (os_root_ends_with_dot(name))
        return os_root_path_error(OS_LIT("RemoveAll"), name,
                                  burrow__os_errno_value(SYSCALL_EINVAL));
    Error e = os_root_do(r, name, 0, NULL, os_root_remove_all_fn, NULL);
    if (os_is_not_exist(e))
        return BURROW_NO_ERROR;
    if (BURROW_FAILED(e))
        return os_root_path_error(OS_LIT("RemoveAll"), name, os_root_underlying(e));
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------- Rename, Link, Symlink */

typedef struct OsRootPairEnv {
    OsRoot *r;
    Str newname;
    bool rename;
    int64_t oldparent;
    Str oldname;
} OsRootPairEnv;

static Error os_root_pair_new_fn(OsRootWalk *w, void *env, int64_t newparent,
                                 Str newname, bool new_ends_in_slash) {
    (void)w;
    OsRootPairEnv *p = (OsRootPairEnv *)env;
    PalErrno pe = PAL_OK;
    if (!p->rename) {
        if (pal_linkat(p->oldparent, os_root_c(p->oldname), newparent,
                       os_root_c(newname), &pe))
            return BURROW_NO_ERROR;
        return burrow__os_errno(pe);
    }
    if (new_ends_in_slash) {
        /* "a" to "b/" is only right when a is a directory. */
        uint32_t mode = 0;
        Error e = os_root_mode_at(p->oldparent, p->oldname, &mode);
        if (BURROW_FAILED(e))
            return e;
        if (mode != PAL_S_IFDIR)
            return burrow__os_errno_value(SYSCALL_ENOTDIR);
    }
    /* Renaming over a directory is refused even where the system would
     * allow it, unless it is the same directory under another name. */
    PalStat nst;
    if (pal_lstatat(newparent, os_root_c(newname), &nst, NULL) &&
        (nst.mode & PAL_S_IFMT) == PAL_S_IFDIR) {
        PalStat ost;
        if (!pal_lstatat(p->oldparent, os_root_c(p->oldname), &ost, &pe))
            return burrow__os_errno(pe);
        if (str_eq(newname, p->oldname) || nst.dev != ost.dev || nst.ino != ost.ino)
            return burrow__os_errno_value(SYSCALL_EEXIST);
    }
    if (pal_renameat(p->oldparent, os_root_c(p->oldname), newparent, os_root_c(newname),
                     &pe))
        return BURROW_NO_ERROR;
    return burrow__os_errno(pe);
}

static Error os_root_pair_old_fn(OsRootWalk *w, void *env, int64_t oldparent,
                                 Str oldname, bool old_ends_in_slash) {
    (void)w;
    (void)old_ends_in_slash;
    OsRootPairEnv *p = (OsRootPairEnv *)env;
    p->oldparent = oldparent;
    p->oldname = oldname;
    unsigned flags = p->rename ? OS_ROOT_CREATING_DIRECTORY : 0;
    return os_root_do(p->r, p->newname, flags, NULL, os_root_pair_new_fn, p);
}

Error os_root_rename(OsRoot *r, Str oldname, Str newname) {
    OsRootPairEnv p = {r, newname, true, PAL_INVALID_HANDLE, {NULL, 0}};
    Error e = os_root_do(r, oldname, 0, NULL, os_root_pair_old_fn, &p);
    if (BURROW_FAILED(e))
        return os_link_error_new(error_allocator(), OS_LIT("renameat"), oldname,
                                 newname, e);
    return BURROW_NO_ERROR;
}

Error os_root_link(OsRoot *r, Str oldname, Str newname) {
    OsRootPairEnv p = {r, newname, false, PAL_INVALID_HANDLE, {NULL, 0}};
    Error e = os_root_do(r, oldname, 0, NULL, os_root_pair_old_fn, &p);
    if (BURROW_FAILED(e))
        return os_link_error_new(error_allocator(), OS_LIT("linkat"), oldname, newname,
                                 e);
    return BURROW_NO_ERROR;
}

typedef struct OsRootSymlinkEnv {
    const char *target;
} OsRootSymlinkEnv;

static Error os_root_symlink_fn(OsRootWalk *w, void *env, int64_t parent, Str name,
                                bool ends_in_slash) {
    (void)w;
    (void)ends_in_slash;
    PalErrno pe = PAL_OK;
    if (pal_symlinkat(((OsRootSymlinkEnv *)env)->target, parent, os_root_c(name), &pe))
        return BURROW_NO_ERROR;
    return burrow__os_errno(pe);
}

Error os_root_symlink(OsRoot *r, Str oldname, Str newname) {
    Error e = BURROW_NO_ERROR;
    OsCPath c;
    if (!burrow__os_cpath(&c, oldname, &e))
        return os_link_error_new(error_allocator(), OS_LIT("symlinkat"), oldname,
                                 newname, e);
    OsRootSymlinkEnv s = {c.p};
    e = os_root_do(r, newname, 0, NULL, os_root_symlink_fn, &s);
    burrow__os_cpath_free(&c);
    if (BURROW_FAILED(e))
        return os_link_error_new(error_allocator(), OS_LIT("symlinkat"), oldname,
                                 newname, e);
    return BURROW_NO_ERROR;
}

#else /* OS_ROOT_BY_NAME */

/* ============================================================== by name */

/* checkPathEscapesInternal: whether name, walked from the root by its
 * name, stays inside it. lstat leaves a link in the last component alone. */
static Error os_root_check_escapes(OsRoot *r, Str name, bool lstat) {
    sync_mutex_lock(&r->mu);
    bool closed = r->closed;
    sync_mutex_unlock(&r->mu);
    if (closed)
        return fs_err_closed;

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *t = arena_allocator(&ar);
    OsRootParts parts;
    bool ends_in_slash;
    Error err = os_root_split(t, name, NULL, 0, NULL, 0, &parts, &ends_in_slash);
    Int i = 0, symlinks = 0;
    Str base = r->name;
    while (BURROW_OK(err) && i < parts.len) {
        if (os_root_is_dotdot(parts.p[i])) {
            Int end = i + 1;
            while (end < parts.len && os_root_is_dotdot(parts.p[end]))
                end++;
            Int count = end - i;
            if (count > i) {
                err = burrow__os_err_path_escapes;
                break;
            }
            memmove(parts.p + i - count, parts.p + end,
                    (size_t)(parts.len - end) * sizeof(Str));
            parts.len -= 2 * count;
            i -= count;
            base = r->name;
            for (Int j = 0; j < i && base.p != NULL; j++)
                base = os_root_join(t, base, parts.p[j]);
            if (base.p == NULL)
                err = burrow_err_out_of_memory;
            continue;
        }

        if (i == parts.len - 1 && lstat && !ends_in_slash)
            break;

        Str next = os_root_join(t, base, parts.p[i]);
        if (next.p == NULL) {
            err = burrow_err_out_of_memory;
            break;
        }
        PalStat st;
        PalErrno pe = PAL_OK;
        if (!pal_lstat((const char *)next.p, &st, &pe)) {
            Error le = burrow__os_errno(pe);
            if (!os_is_not_exist(le))
                err = le;
            break;
        }
        if ((st.mode & PAL_S_IFMT) == PAL_S_IFLNK) {
            Error re = BURROW_NO_ERROR;
            Str link = os_readlink(t, next, &re);
            if (BURROW_FAILED(re)) {
                err = burrow__os_err_path_escapes;
                break;
            }
            if (++symlinks > OS_ROOT_MAX_SYMLINKS) {
                err = burrow__os_err_root_symlinks;
                break;
            }
            OsRootParts np;
            bool new_ends_in_slash;
            err = os_root_split(t, link, parts.p, i, parts.p + i + 1, parts.len - i - 1,
                                &np, &new_ends_in_slash);
            if (BURROW_FAILED(err))
                break;
            if (i == parts.len - 1 && new_ends_in_slash)
                ends_in_slash = true;
            parts = np;
            continue;
        }
        if ((st.mode & PAL_S_IFMT) != PAL_S_IFDIR && i < parts.len - 1) {
            err = burrow__os_errno_value(SYSCALL_ENOTDIR);
            break;
        }
        base = next;
        i++;
    }
    arena_free(&ar);
    return err;
}

/* The root's name and name joined, from the heap. */
static Str os_root_full(OsRoot *r, Str name) {
    return os_root_join(heap_allocator(), r->name, name);
}

static OsRoot *os_root_by_name(Alloc *a, Str name, Error *e) {
    Error se = BURROW_NO_ERROR;
    Arena ar;
    arena_init(&ar, NULL, 0);
    OsFileInfo fi = os_stat(arena_allocator(&ar), name, &se);
    bool dir = BURROW_OK(se) && fi.vt->is_dir(fi.data);
    arena_free(&ar);
    if (BURROW_FAILED(se)) {
        *e = os_root_underlying(se);
        return NULL;
    }
    if (!dir) {
        *e = burrow__os_err_root_not_dir;
        return NULL;
    }
    OsRoot *r = os_root_new(a, PAL_INVALID_HANDLE, name);
    if (r == NULL)
        *e = burrow_err_out_of_memory;
    return r;
}

OsRoot *os_open_root(Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Error e = BURROW_NO_ERROR;
    if (name.len == 0) {
        BURROW_OUT(err, os_root_path_error(OS_LIT("open"), name,
                                           burrow__os_errno_value(SYSCALL_ENOENT)));
        return NULL;
    }
    OsRoot *r = os_root_by_name(a, name, &e);
    if (r == NULL)
        BURROW_OUT(err, os_root_path_error(OS_LIT("open"), name, e));
    return r;
}

OsRoot *os_root_open_root(OsRoot *r, Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Error e = os_root_check_escapes(r, name, false);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, os_root_path_error(OS_LIT("openat"), name, e));
        return NULL;
    }
    Str full = os_root_full(r, name);
    if (full.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    OsRoot *nr = os_root_by_name(a, full, &e);
    os_root_str_free(full);
    if (nr == NULL)
        BURROW_OUT(err, os_root_path_error(OS_LIT("openat"), name, e));
    return nr;
}

static OsFile *os_root_open_file_nolog(OsRoot *r, Alloc *a, Str name, Int flag,
                                       OsFileMode perm, Error *err) {
    Error e = os_root_check_escapes(r, name, false);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, os_root_path_error(OS_LIT("openat"), name, e));
        return NULL;
    }
    Str full = os_root_full(r, name);
    if (full.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    OsFile *f = os_open_file(a, full, flag, perm, &e);
    os_root_str_free(full);
    if (f == NULL)
        BURROW_OUT(err,
                   os_root_path_error(OS_LIT("openat"), name, os_root_underlying(e)));
    return f;
}

static OsFileInfo os_root_stat_op(OsRoot *r, Alloc *a, Str name, bool lstat,
                                  Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    OsFileInfo fi = {NULL, NULL};
    Error e = os_root_check_escapes(r, name, lstat);
    if (BURROW_OK(e)) {
        Str full = os_root_full(r, name);
        if (full.p == NULL)
            e = burrow_err_out_of_memory;
        else
            fi = lstat ? os_lstat(a, full, &e) : os_stat(a, full, &e);
        os_root_str_free(full);
    }
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err,
                   os_root_path_error(OS_LIT("statat"), name, os_root_underlying(e)));
        return (OsFileInfo){NULL, NULL};
    }
    return fi;
}

/* The shape of most of root_noopenat.go: check, join, call, and the error
 * with op and the name the root was given. */
typedef enum OsRootNameOp {
    OS_ROOT_N_CHMOD,
    OS_ROOT_N_CHOWN,
    OS_ROOT_N_LCHOWN,
    OS_ROOT_N_CHTIMES,
    OS_ROOT_N_MKDIR,
    OS_ROOT_N_REMOVE,
    OS_ROOT_N_REMOVE_ALL
} OsRootNameOp;

typedef struct OsRootNameArgs {
    OsFileMode mode;
    Int uid;
    Int gid;
    Time atime;
    Time mtime;
} OsRootNameArgs;

static Error os_root_by_name_op(OsRoot *r, Str name, OsRootNameOp op, Str op_name,
                                bool lstat, const OsRootNameArgs *args) {
    Error e = os_root_check_escapes(r, name, lstat);
    if (BURROW_FAILED(e))
        return os_root_path_error(op_name, name, e);
    Str full = os_root_full(r, name);
    if (full.p == NULL)
        return burrow_err_out_of_memory;
    switch (op) {
    case OS_ROOT_N_CHMOD:
        e = os_chmod(full, args->mode);
        break;
    case OS_ROOT_N_CHOWN:
        e = os_chown(full, args->uid, args->gid);
        break;
    case OS_ROOT_N_LCHOWN:
        e = os_lchown(full, args->uid, args->gid);
        break;
    case OS_ROOT_N_CHTIMES:
        e = os_chtimes(full, args->atime, args->mtime);
        break;
    case OS_ROOT_N_MKDIR:
        e = os_mkdir(full, args->mode);
        break;
    case OS_ROOT_N_REMOVE:
        e = os_remove(full);
        break;
    case OS_ROOT_N_REMOVE_ALL:
        e = os_remove_all(full);
        break;
    default:
        break;
    }
    os_root_str_free(full);
    if (BURROW_FAILED(e))
        return os_root_path_error(op_name, name, os_root_underlying(e));
    return BURROW_NO_ERROR;
}

Error os_root_chmod(OsRoot *r, Str name, OsFileMode mode) {
    OsRootNameArgs a = {.mode = mode};
    return os_root_by_name_op(r, name, OS_ROOT_N_CHMOD, OS_LIT("chmodat"), false, &a);
}

Error os_root_chown(OsRoot *r, Str name, Int uid, Int gid) {
    OsRootNameArgs a = {.uid = uid, .gid = gid};
    return os_root_by_name_op(r, name, OS_ROOT_N_CHOWN, OS_LIT("chownat"), false, &a);
}

Error os_root_lchown(OsRoot *r, Str name, Int uid, Int gid) {
    OsRootNameArgs a = {.uid = uid, .gid = gid};
    return os_root_by_name_op(r, name, OS_ROOT_N_LCHOWN, OS_LIT("lchownat"), true, &a);
}

Error os_root_chtimes(OsRoot *r, Str name, Time atime, Time mtime) {
    OsRootNameArgs a = {.atime = atime, .mtime = mtime};
    return os_root_by_name_op(r, name, OS_ROOT_N_CHTIMES, OS_LIT("chtimesat"), false,
                              &a);
}

static Error os_root_mkdir_impl(OsRoot *r, Str name, OsFileMode perm) {
    Error e = os_root_check_escapes(r, name, false);
    if (BURROW_FAILED(e))
        return os_root_path_error(OS_LIT("mkdirat"), name, e);
    if (name.len == 0)
        return os_root_path_error(OS_LIT("mkdirat"), name,
                                  burrow__os_errno_value(SYSCALL_ENOENT));
    OsRootNameArgs a = {.mode = perm};
    return os_root_by_name_op(r, name, OS_ROOT_N_MKDIR, OS_LIT("mkdirat"), false, &a);
}

static Error os_root_mkdir_all_impl(OsRoot *r, Str name, OsFileMode perm) {
    Error e = os_root_check_escapes(r, name, false);
    if (os_root_same(e, burrow__os_err_path_escapes))
        return os_root_path_error(OS_LIT("mkdirat"), name, e);
    if (name.len == 0)
        return os_root_path_error(OS_LIT("mkdirat"), name,
                                  burrow__os_errno_value(SYSCALL_ENOENT));
    Byte sep = (Byte)OS_PATH_SEPARATOR;
    Str prefix = burrow__os_cat3(heap_allocator(), r->name, str_from_bytes(&sep, 1),
                                 (Str){NULL, 0});
    if (prefix.p == NULL)
        return burrow_err_out_of_memory;
    Str full = burrow__os_cat3(heap_allocator(), prefix, name, (Str){NULL, 0});
    if (full.p == NULL) {
        os_root_str_free(prefix);
        return burrow_err_out_of_memory;
    }
    e = os_mkdir_all(full, perm);
    os_root_str_free(full);
    if (BURROW_FAILED(e)) {
        if (e.vt != NULL && e.vt->self_type == TYPE_FS_PATH_ERROR) {
            const FsPathError *pe = (const FsPathError *)e.data;
            Str path = pe->path;
            if (path.len >= prefix.len &&
                memcmp(path.p, prefix.p, (size_t)prefix.len) == 0)
                path = str_from_bytes(path.p + prefix.len, path.len - prefix.len);
            e = os_root_path_error(OS_LIT("mkdirat"), path, pe->err);
        } else {
            e = os_root_path_error(OS_LIT("mkdirat"), name, os_root_underlying(e));
        }
    }
    os_root_str_free(prefix);
    return e;
}

Error os_root_remove(OsRoot *r, Str name) {
    Error e = os_root_check_escapes(r, name, true);
    if (BURROW_FAILED(e))
        return os_root_path_error(OS_LIT("removeat"), name, e);
    if (os_root_ends_with_dot(name)) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        bool dot = os_root_is_dot(filepath_clean(arena_allocator(&ar), name));
        arena_free(&ar);
        if (dot)
            return os_root_path_error(OS_LIT("removeat"), name,
                                      burrow__os_err_path_escapes);
    }
    return os_root_by_name_op(r, name, OS_ROOT_N_REMOVE, OS_LIT("removeat"), true,
                              NULL);
}

Error os_root_remove_all(OsRoot *r, Str name) {
    while (name.len > 0 && os_is_path_separator(name.p[name.len - 1]))
        name.len--;
    if (os_root_ends_with_dot(name))
        return os_root_path_error(OS_LIT("RemoveAll"), name,
                                  burrow__os_errno_value(SYSCALL_EINVAL));
    Error e = os_root_check_escapes(r, name, true);
    if (BURROW_FAILED(e)) {
        if (os_root_is_errno(e, SYSCALL_ENOTDIR))
            return BURROW_NO_ERROR;
        return os_root_path_error(OS_LIT("RemoveAll"), name, e);
    }
    return os_root_by_name_op(r, name, OS_ROOT_N_REMOVE_ALL, OS_LIT("RemoveAll"), true,
                              NULL);
}

Str os_root_readlink(OsRoot *r, Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Error e = os_root_check_escapes(r, name, true);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, os_root_path_error(OS_LIT("readlinkat"), name, e));
        return (Str){NULL, 0};
    }
    Str full = os_root_full(r, name);
    if (full.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (Str){NULL, 0};
    }
    Str target = os_readlink(a, full, &e);
    os_root_str_free(full);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(
            err, os_root_path_error(OS_LIT("readlinkat"), name, os_root_underlying(e)));
        return (Str){NULL, 0};
    }
    return target;
}

Error os_root_rename(OsRoot *r, Str oldname, Str newname) {
    Error e = os_root_check_escapes(r, oldname, true);
    if (BURROW_FAILED(e))
        return os_root_path_error(OS_LIT("renameat"), oldname, e);
    e = os_root_check_escapes(r, newname, true);
    if (BURROW_FAILED(e))
        return os_root_path_error(OS_LIT("renameat"), newname, e);
    Str fo = os_root_full(r, oldname);
    Str fn = os_root_full(r, newname);
    e = fo.p == NULL || fn.p == NULL ? burrow_err_out_of_memory : os_rename(fo, fn);
    os_root_str_free(fo);
    os_root_str_free(fn);
    if (BURROW_FAILED(e))
        return os_link_error_new(error_allocator(), OS_LIT("renameat"), oldname,
                                 newname, os_root_underlying(e));
    return BURROW_NO_ERROR;
}

Error os_root_link(OsRoot *r, Str oldname, Str newname) {
    Error e = os_root_check_escapes(r, oldname, true);
    if (BURROW_FAILED(e))
        return os_root_path_error(OS_LIT("linkat"), oldname, e);
    Str fo = os_root_full(r, oldname);
    if (fo.p == NULL)
        return burrow_err_out_of_memory;
    OsCPath c;
    PalStat st;
    if (burrow__os_cpath(&c, fo, NULL)) {
        bool link = pal_lstat(c.p, &st, NULL) && (st.mode & PAL_S_IFMT) == PAL_S_IFLNK;
        burrow__os_cpath_free(&c);
        if (link) {
            os_root_str_free(fo);
            return os_root_path_error(OS_LIT("linkat"), oldname,
                                      burrow__os_err_root_hard_link);
        }
    }
    e = os_root_check_escapes(r, newname, true);
    if (BURROW_FAILED(e)) {
        os_root_str_free(fo);
        return os_root_path_error(OS_LIT("linkat"), newname, e);
    }
    Str fn = os_root_full(r, newname);
    e = fn.p == NULL ? burrow_err_out_of_memory : os_link(fo, fn);
    os_root_str_free(fo);
    os_root_str_free(fn);
    if (BURROW_FAILED(e))
        return os_link_error_new(error_allocator(), OS_LIT("linkat"), oldname, newname,
                                 os_root_underlying(e));
    return BURROW_NO_ERROR;
}

Error os_root_symlink(OsRoot *r, Str oldname, Str newname) {
    Error e = os_root_check_escapes(r, newname, true);
    if (BURROW_FAILED(e))
        return os_root_path_error(OS_LIT("symlinkat"), newname, e);
    Str fn = os_root_full(r, newname);
    if (fn.p == NULL)
        return burrow_err_out_of_memory;
    e = os_symlink(oldname, fn);
    os_root_str_free(fn);
    if (BURROW_FAILED(e))
        return os_link_error_new(error_allocator(), OS_LIT("symlinkat"), oldname,
                                 newname, os_root_underlying(e));
    return BURROW_NO_ERROR;
}

#endif /* OS_ROOT_BY_NAME */

OsFileInfo os_root_stat(OsRoot *r, Alloc *a, Str name, Error *err) {
    return os_root_stat_op(r, a, name, false, err);
}

OsFileInfo os_root_lstat(OsRoot *r, Alloc *a, Str name, Error *err) {
    return os_root_stat_op(r, a, name, true, err);
}

/* ----------------------------------------------------------------- rootFS */

static const Type os_root_fs_desc = {
    {(const Byte *)"rootFS", 6},
    {(const Byte *)"os", 2},
    KIND_STRUCT,
    (uint32_t)sizeof(OsRoot),
    (uint16_t)_Alignof(OsRoot),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6f737266U, /* "osrf" */
    NULL,
};

/* isValidRootFSPath: an io/fs name, and on Windows one with no backslash. */
static bool os_root_fs_valid(Str name, Str op, Error *err) {
    bool ok = fs_valid_path(name);
#if defined(BURROW_OS_WINDOWS)
    if (ok && memchr(name.p, '\\', (size_t)name.len) != NULL)
        ok = false;
#endif
    if (!ok)
        BURROW_OUT(err, os_root_path_error(op, name, fs_err_invalid));
    return ok;
}

static FsFile os_rfs_open(void *self, Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!os_root_fs_valid(name, OS_LIT("open"), err))
        return (FsFile){NULL, NULL};
    OsFile *f = os_root_open((OsRoot *)self, a, name, err);
    if (f == NULL)
        return (FsFile){NULL, NULL};
    return os_file_as_fs_file(f);
}

static int os_rfs_entry_cmp(void *env, const void *x, const void *y) {
    (void)env;
    const FsDirEntry *d1 = (const FsDirEntry *)x;
    const FsDirEntry *d2 = (const FsDirEntry *)y;
    return (int)strings_compare(d1->vt->name(d1->data), d2->vt->name(d2->data));
}

static Slice os_rfs_read_dir(void *self, Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!os_root_fs_valid(name, OS_LIT("readdir"), err))
        return slice_nil(TYPE_FS_DIR_ENTRY);
    OsFile *f = os_root_open((OsRoot *)self, heap_allocator(), name, err);
    if (f == NULL)
        return slice_nil(TYPE_FS_DIR_ENTRY);
    Slice dirs = os_file_read_dir(f, a, -1, err);
    os_file_free(f);
    slices_sort_func(dirs, BURROW_FN(SlicesCmpFunc, os_rfs_entry_cmp, NULL));
    return dirs;
}

static Slice os_rfs_read_file(void *self, Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!os_root_fs_valid(name, OS_LIT("readfile"), err))
        return slice_nil(TYPE_BYTE);
    return os_root_read_file((OsRoot *)self, a, name, err);
}

static Str os_rfs_read_link(void *self, Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!os_root_fs_valid(name, OS_LIT("readlink"), err))
        return (Str){NULL, 0};
    return os_root_readlink((OsRoot *)self, a, name, err);
}

static FsFileInfo os_rfs_stat(void *self, Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!os_root_fs_valid(name, OS_LIT("stat"), err))
        return (FsFileInfo){NULL, NULL};
    return os_root_stat((OsRoot *)self, a, name, err);
}

static FsFileInfo os_rfs_lstat(void *self, Alloc *a, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!os_root_fs_valid(name, OS_LIT("lstat"), err))
        return (FsFileInfo){NULL, NULL};
    return os_root_lstat((OsRoot *)self, a, name, err);
}

static const FsVT os_root_fs_vt = {
    &os_root_fs_desc,
    os_rfs_open,
    os_rfs_read_dir,
    os_rfs_read_file,
    os_rfs_stat,
    NULL,
    NULL,
    os_rfs_read_link,
    os_rfs_lstat,
};

Fs os_root_fs(OsRoot *r) {
    return (Fs){&os_root_fs_vt, r};
}

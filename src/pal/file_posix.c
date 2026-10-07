/* Files on everything but Windows.
 *
 * Almost all of this is one system call each, and the work is in the edges:
 * retrying the calls a signal can interrupt, making every descriptor close on
 * exec, keeping a single read or write under the size macOS refuses, and
 * turning a struct stat, whose field names differ between Linux and the BSDs,
 * into a PalStat. Reading a directory is not here, because that is the one call
 * that is different on every kernel, and it lives in dir_linux.c, dir_darwin.c
 * and dir_bsd.c.
 *
 * _FILE_OFFSET_BITS is for 32 bit Linux, where off_t is 32 bits without it and
 * a file past two gigabytes cannot be seeked in. The others are for utimensat,
 * pipe2 and F_DUPFD_CLOEXEC, which a strict build hides. FreeBSD and
 * DragonFly are the other way round: they show everything by default, and
 * asking for X/Open there hides pipe2, so they get no _XOPEN_SOURCE.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#if !defined(__FreeBSD__) && !defined(__DragonFly__)
#define _XOPEN_SOURCE 700
#endif
#define _DEFAULT_SOURCE 1
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(BURROW_OS_WASI)
#include <stdlib.h>
#include <wasi/api.h>
#endif

/* macOS fails a read or write of more than INT_MAX bytes with EINVAL, and Go
 * caps every one at a gigabyte on every platform for that reason. A short read
 * or write is already something every caller handles, so the cap costs
 * nothing. */
#define FILE_MAX_RW ((int64_t)1 << 30)

static bool file_fail(PalErrno *err) {
    BURROW_OUT(err, burrow__pal_errno(errno));
    return false;
}

static int64_t file_fail_n(PalErrno *err) {
    BURROW_OUT(err, burrow__pal_errno(errno));
    return -1;
}

static bool fd_ok(int64_t fd, PalErrno *err) {
    if (fd < 0 || fd > INT32_MAX) {
        BURROW_OUT(err, PAL_EBADF);
        return false;
    }
    return true;
}

static bool count_ok(const void *buf, int64_t n, PalErrno *err) {
    if (n < 0 || (n > 0 && buf == NULL)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    return true;
}

static int open_flags(uint32_t flags) {
    /* The casts are for cosmopolitan, whose O_ constants are unsigned
     * variables filled in at startup for whichever system it finds itself
     * on. */
    int o = (int)O_CLOEXEC;
    switch (flags & PAL_O_ACCMODE) {
    case PAL_O_WRONLY:
        o |= (int)O_WRONLY;
        break;
    case PAL_O_RDWR:
        o |= (int)O_RDWR;
        break;
    default:
        o |= (int)O_RDONLY;
        break;
    }
    if (flags & PAL_O_APPEND)
        o |= (int)O_APPEND;
    if (flags & PAL_O_CREATE)
        o |= (int)O_CREAT;
    if (flags & PAL_O_EXCL)
        o |= (int)O_EXCL;
    if (flags & PAL_O_TRUNC)
        o |= (int)O_TRUNC;
    if (flags & PAL_O_SYNC)
        o |= (int)O_SYNC;
    if (flags & PAL_O_NONBLOCK)
        o |= (int)O_NONBLOCK;
    if (flags & PAL_O_DIRECTORY)
        o |= (int)O_DIRECTORY;
    if (flags & PAL_O_NOFOLLOW)
        o |= (int)O_NOFOLLOW;
    return o;
}

int64_t pal_open(const char *path, uint32_t flags, uint32_t mode, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (path == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return PAL_INVALID_HANDLE;
    }

    int o = open_flags(flags);
    for (;;) {
        int fd = open(path, o, (mode_t)(mode & 07777));
        if (fd >= 0)
            return fd;
        if (errno != EINTR)
            return file_fail_n(err);
    }
}

bool pal_close(int64_t fd, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!fd_ok(fd, err))
        return false;

    /* Not retried on EINTR. Linux has already released the descriptor by the
     * time close reports that, and a retry would close whatever another thread
     * has opened in the meantime under the same number. */
    if (close((int)fd) != 0 && errno != EINTR)
        return file_fail(err);
    return true;
}

int64_t pal_read(int64_t fd, void *buf, int64_t n, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!fd_ok(fd, err) || !count_ok(buf, n, err))
        return -1;
    if (n > FILE_MAX_RW)
        n = FILE_MAX_RW;

    for (;;) {
        ssize_t got = read((int)fd, buf, (size_t)n);
        if (got >= 0)
            return (int64_t)got;
        if (errno != EINTR)
            return file_fail_n(err);
    }
}

int64_t pal_pread(int64_t fd, void *buf, int64_t n, int64_t off, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!fd_ok(fd, err) || !count_ok(buf, n, err))
        return -1;
    if (off < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    if (n > FILE_MAX_RW)
        n = FILE_MAX_RW;

    for (;;) {
        ssize_t got = pread((int)fd, buf, (size_t)n, (off_t)off);
        if (got >= 0)
            return (int64_t)got;
        if (errno != EINTR)
            return file_fail_n(err);
    }
}

int64_t pal_write(int64_t fd, const void *buf, int64_t n, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!fd_ok(fd, err) || !count_ok(buf, n, err))
        return -1;
    if (n > FILE_MAX_RW)
        n = FILE_MAX_RW;

    for (;;) {
        ssize_t put = write((int)fd, buf, (size_t)n);
        if (put >= 0)
            return (int64_t)put;
        if (errno != EINTR)
            return file_fail_n(err);
    }
}

int64_t pal_pwrite(int64_t fd, const void *buf, int64_t n, int64_t off, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!fd_ok(fd, err) || !count_ok(buf, n, err))
        return -1;
    if (off < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    if (n > FILE_MAX_RW)
        n = FILE_MAX_RW;

    for (;;) {
        ssize_t put = pwrite((int)fd, buf, (size_t)n, (off_t)off);
        if (put >= 0)
            return (int64_t)put;
        if (errno != EINTR)
            return file_fail_n(err);
    }
}

int64_t pal_seek(int64_t fd, int64_t off, int32_t whence, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!fd_ok(fd, err))
        return -1;

    int w;
    switch (whence) {
    case PAL_SEEK_SET:
        w = SEEK_SET;
        break;
    case PAL_SEEK_CUR:
        w = SEEK_CUR;
        break;
    case PAL_SEEK_END:
        w = SEEK_END;
        break;
    default:
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }

    /* In brackets because wasi-libc makes lseek a macro with a GNU statement
     * expression in it, which -Wpedantic objects to. */
    off_t at = (lseek)((int)fd, (off_t)off, w);
    if (at < 0)
        return file_fail_n(err);
    return (int64_t)at;
}

bool pal_fsync(int64_t fd, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!fd_ok(fd, err))
        return false;

#if defined(F_FULLFSYNC)
    /* On macOS fsync only gets the data as far as the drive, which may keep it
     * in a cache of its own. F_FULLFSYNC is what gets it onto the platter, and
     * it is what Go's File.Sync asks for there. Some filesystems refuse it, and
     * those get the plain call. */
    if (fcntl((int)fd, F_FULLFSYNC) == 0)
        return true;
    if (errno != ENOTSUP && errno != ENOTTY && errno != EINVAL)
        return file_fail(err);
#endif
    for (;;) {
        if (fsync((int)fd) == 0)
            return true;
        if (errno != EINTR)
            return file_fail(err);
    }
}

bool pal_ftruncate(int64_t fd, int64_t size, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!fd_ok(fd, err))
        return false;
    if (size < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    for (;;) {
        if (ftruncate((int)fd, (off_t)size) == 0)
            return true;
        if (errno != EINTR)
            return file_fail(err);
    }
}

/* The three times and the birth time have different field names on Linux and on
 * the BSDs, and Linux has no birth time in struct stat at all. */
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
#define ST_ATIM st_atimespec
#define ST_MTIM st_mtimespec
#define ST_CTIM st_ctimespec
#define ST_BIRTHTIM st_birthtimespec
#elif defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD)
#define ST_ATIM st_atim
#define ST_MTIM st_mtim
#define ST_CTIM st_ctim
#define ST_BIRTHTIM st_birthtim
#else
#define ST_ATIM st_atim
#define ST_MTIM st_mtim
#define ST_CTIM st_ctim
#endif

static void stat_from(const struct stat *st, PalStat *out) {
    *out = (PalStat){0};
    out->size = (int64_t)st->st_size;
    /* The PAL_S_ numbering is POSIX's, so nothing is translated. */
    out->mode = (uint32_t)st->st_mode;
#if defined(BURROW_OS_WASI)
    /* WASI has no permissions. Go's wasip1 gives a directory 0700 and anything
     * else 0600, since programs expect some, and so does this. */
    out->mode = (out->mode & ~(uint32_t)07777) | (S_ISDIR(st->st_mode) ? 0700u : 0600u);
#endif
    out->uid = (uint32_t)st->st_uid;
    out->gid = (uint32_t)st->st_gid;
    out->dev = (uint64_t)st->st_dev;
    out->ino = (uint64_t)st->st_ino;
    out->rdev = (uint64_t)st->st_rdev;
    out->nlink = (uint64_t)st->st_nlink;
    out->blocks = (int64_t)st->st_blocks;
    out->blksize = (int64_t)st->st_blksize;
    out->atime_sec = (int64_t)st->ST_ATIM.tv_sec;
    out->atime_nsec = (int32_t)st->ST_ATIM.tv_nsec;
    out->mtime_sec = (int64_t)st->ST_MTIM.tv_sec;
    out->mtime_nsec = (int32_t)st->ST_MTIM.tv_nsec;
    out->ctime_sec = (int64_t)st->ST_CTIM.tv_sec;
    out->ctime_nsec = (int32_t)st->ST_CTIM.tv_nsec;
#if defined(ST_BIRTHTIM)
    out->btime_sec = (int64_t)st->ST_BIRTHTIM.tv_sec;
    out->btime_nsec = (int32_t)st->ST_BIRTHTIM.tv_nsec;
#endif
}

bool pal_stat(const char *path, PalStat *out, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (path == NULL || out == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    struct stat st;
    for (;;) {
        if (stat(path, &st) == 0)
            break;
        if (errno != EINTR)
            return file_fail(err);
    }
    stat_from(&st, out);
    return true;
}

bool pal_lstat(const char *path, PalStat *out, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (path == NULL || out == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    struct stat st;
    for (;;) {
        if (lstat(path, &st) == 0)
            break;
        if (errno != EINTR)
            return file_fail(err);
    }
    stat_from(&st, out);
    return true;
}

bool pal_fstat(int64_t fd, PalStat *out, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!fd_ok(fd, err))
        return false;
    if (out == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    struct stat st;
#if defined(BURROW_OS_LINUX) && defined(AT_EMPTY_PATH)
    /* The same call through fstatat. Under qemu-user on s390x, fstat comes
     * back without the nanoseconds and fstatat on the same descriptor has
     * them, so a file's Stat would disagree with Stat on its name. A kernel too
     * old for AT_EMPTY_PATH says ENOENT and gets plain fstat. */
    for (;;) {
        if (fstatat((int)fd, "", &st, AT_EMPTY_PATH) == 0) {
            stat_from(&st, out);
            return true;
        }
        if (errno != EINTR)
            break;
    }
    if (errno != ENOENT && errno != EINVAL)
        return file_fail(err);
#endif
    for (;;) {
        if (fstat((int)fd, &st) == 0)
            break;
        if (errno != EINTR)
            return file_fail(err);
    }
    stat_from(&st, out);
    return true;
}

static bool posix_path_ok(const char *path, PalErrno *err) {
    if (path == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    return true;
}

bool pal_unlink(const char *path, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(path, err))
        return false;
    return unlink(path) == 0 || file_fail(err);
}

bool pal_rename(const char *from, const char *to, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(from, err) || !posix_path_ok(to, err))
        return false;
    return rename(from, to) == 0 || file_fail(err);
}

bool pal_rename_replace(const char *from, const char *to, PalErrno *err) {
    return pal_rename(from, to, err);
}

bool pal_mkdir(const char *path, uint32_t mode, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(path, err))
        return false;
    return mkdir(path, (mode_t)(mode & 07777)) == 0 || file_fail(err);
}

bool pal_rmdir(const char *path, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(path, err))
        return false;
    return rmdir(path) == 0 || file_fail(err);
}

bool pal_chdir(const char *path, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(path, err))
        return false;
    return chdir(path) == 0 || file_fail(err);
}

#if defined(BURROW_OS_WASI)

/* A wasip1 descriptor has no way back to its path. Go's os keeps the path it
 * opened the file with and changes to that, and burrow's os does the same. */
bool pal_fchdir(int64_t fd, PalErrno *err) {
    (void)fd;
    BURROW_OUT(err, PAL_ENOSYS);
    return false;
}

#else

bool pal_fchdir(int64_t fd, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!fd_ok(fd, err))
        return false;
    return fchdir((int)fd) == 0 || file_fail(err);
}

#endif

int64_t pal_getcwd(char *buf, int64_t cap, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (buf == NULL || cap <= 0) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    if (getcwd(buf, (size_t)cap) == NULL) {
        file_fail(err);
        return -1;
    }
    int64_t n = 0;
    while (buf[n] != 0)
        n++;
    return n;
}

#if defined(BURROW_OS_WASI)

/* Go's syscall keeps its own working directory on wasip1. It starts at $PWD,
 * cleaned and made absolute, or at the name of the first preopened directory
 * when there is no $PWD. wasi-libc keeps one too and starts it at "/", so it is
 * moved to where Go's would be before main runs. A $PWD that is not a directory
 * leaves it at "/", where Go would keep the name and fail on the first relative
 * path. */
__attribute__((constructor)) static void wasi_start_cwd(void) {
    char path[4096];
    const char *pwd = getenv("PWD");
    if (pwd != NULL && pwd[0] != 0) {
        /* Go's joinPath("/", pwd): every . goes, and every .. takes the name
         * before it with it, or nothing at the top. */
        size_t n = 0;
        const char *p = pwd;
        while (*p != 0) {
            while (*p == '/')
                p++;
            const char *e = p;
            while (*e != 0 && *e != '/')
                e++;
            size_t len = (size_t)(e - p);
            if (len == 0 || (len == 1 && p[0] == '.')) {
                /* nothing */
            } else if (len == 2 && p[0] == '.' && p[1] == '.') {
                while (n > 0 && path[n - 1] != '/')
                    n--;
                if (n > 0)
                    n--;
            } else {
                if (n + 1 + len >= sizeof path)
                    return;
                path[n] = '/';
                memcpy(path + n + 1, p, len);
                n += 1 + len;
            }
            p = e;
        }
        if (n == 0)
            path[n++] = '/';
        path[n] = 0;
        (void)chdir(path);
        return;
    }
    for (__wasi_fd_t fd = 3;; fd++) {
        __wasi_prestat_t st;
        __wasi_errno_t e = __wasi_fd_prestat_get(fd, &st);
        if (e == __WASI_ERRNO_BADF)
            return;
        if (e != 0 || st.tag != __WASI_PREOPENTYPE_DIR)
            continue;
        size_t len = st.u.dir.pr_name_len;
        if (len >= sizeof path ||
            __wasi_fd_prestat_dir_name(fd, (uint8_t *)path, len) != 0)
            return;
        path[len] = 0;
        (void)chdir(path);
        return;
    }
}

#endif

int64_t pal_temp_dir(char *buf, int64_t cap, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    const char *d = NULL;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++)
        if (strncmp(*env, "TMPDIR=", 7) == 0)
            d = *env + 7;
    if (d == NULL || *d == 0)
        d = "/tmp";
    size_t n = strlen(d);
    if (buf == NULL || cap <= 0 || n >= (size_t)cap) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    memcpy(buf, d, n + 1);
    return (int64_t)n;
}

/* There are no DOS devices outside Windows. */
bool pal_is_dos_device_name(const char *name, int64_t n) {
    (void)name;
    (void)n;
    return false;
}

int64_t pal_full_path(const char *path, char *buf, int64_t cap, PalErrno *err) {
    (void)path;
    (void)buf;
    (void)cap;
    BURROW_OUT(err, PAL_ENOTSUP);
    return -1;
}

int64_t pal_find_name(const char *path, char *buf, int64_t cap, PalErrno *err) {
    (void)path;
    (void)buf;
    (void)cap;
    BURROW_OUT(err, PAL_ENOTSUP);
    return -1;
}

bool pal_link(const char *from, const char *to, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(from, err) || !posix_path_ok(to, err))
        return false;
    /* linkat with no flags rather than link, because link follows a symlink
     * named by from on some systems and not on others, and linkat without
     * AT_SYMLINK_FOLLOW never does. That is Go's choice too. */
    return linkat(AT_FDCWD, from, AT_FDCWD, to, 0) == 0 || file_fail(err);
}

bool pal_symlink(const char *target, const char *path, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(target, err) || !posix_path_ok(path, err))
        return false;
    return symlink(target, path) == 0 || file_fail(err);
}

int64_t pal_readlink(const char *path, char *buf, int64_t cap, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(path, err) || !count_ok(buf, cap, err))
        return -1;

    ssize_t n = readlink(path, buf, (size_t)cap);
    if (n < 0)
        return file_fail_n(err);
    /* readlink truncates silently, so a result that fills the buffer may be the
     * front of a longer one. The caller cannot tell, so it is told. */
    if ((int64_t)n >= cap) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    return (int64_t)n;
}

#if defined(BURROW_OS_WASI)

/* wasip1 has no modes to change. Go's Chmod there only checks that the file is
 * there, so this does the same. */
bool pal_chmod(const char *path, uint32_t mode, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    (void)mode;
    if (!posix_path_ok(path, err))
        return false;
    struct stat st;
    return stat(path, &st) == 0 || file_fail(err);
}

#else

bool pal_chmod(const char *path, uint32_t mode, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(path, err))
        return false;
    return chmod(path, (mode_t)(mode & 07777)) == 0 || file_fail(err);
}

#endif

int pal_fd_wait(int64_t fd, bool write, int64_t cancel, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    struct pollfd p[2];
    p[0].fd = (int)fd;
    p[0].events = write ? POLLOUT : POLLIN;
    p[1].fd = (int)cancel;
    p[1].events = POLLIN;
    for (;;) {
        p[0].revents = 0;
        p[1].revents = 0;
        if (poll(p, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            BURROW_OUT(err, burrow__pal_errno(errno));
            return -1;
        }
        if (p[1].revents != 0)
            return 0;
        if (p[0].revents != 0)
            return 1;
    }
}

bool pal_eaccess(const char *path, uint32_t mode, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(path, err))
        return false;
#if defined(AT_EACCESS)
    return faccessat(AT_FDCWD, path, (int)(mode & 7), AT_EACCESS) == 0 ||
           file_fail(err);
#else
    (void)mode;
    BURROW_OUT(err, PAL_ENOSYS);
    return false;
#endif
}

#if defined(BURROW_OS_WASI)

/* wasip1 has no owners. Go answers ENOSYS for all of these. */
static bool file_nosys(PalErrno *err) {
    BURROW_OUT(err, PAL_ENOSYS);
    return false;
}

bool pal_chown(const char *path, int64_t uid, int64_t gid, PalErrno *err) {
    (void)path;
    (void)uid;
    (void)gid;
    return file_nosys(err);
}

bool pal_lchown(const char *path, int64_t uid, int64_t gid, PalErrno *err) {
    (void)path;
    (void)uid;
    (void)gid;
    return file_nosys(err);
}

#else

bool pal_chown(const char *path, int64_t uid, int64_t gid, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(path, err))
        return false;
    uid_t u = uid < 0 ? (uid_t)-1 : (uid_t)uid;
    gid_t g = gid < 0 ? (gid_t)-1 : (gid_t)gid;
    return chown(path, u, g) == 0 || file_fail(err);
}

bool pal_lchown(const char *path, int64_t uid, int64_t gid, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(path, err))
        return false;
    uid_t u = uid < 0 ? (uid_t)-1 : (uid_t)uid;
    gid_t g = gid < 0 ? (gid_t)-1 : (gid_t)gid;
    for (;;) {
        if (lchown(path, u, g) == 0)
            return true;
        if (errno != EINTR)
            return file_fail(err);
    }
}

#endif

#if defined(BURROW_OS_WASI)

/* As pal_chmod: Go's Fchmod on wasip1 is an Fstat. */
bool pal_fchmod(int64_t fd, uint32_t mode, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    (void)mode;
    if (!fd_ok(fd, err))
        return false;
    struct stat st;
    return fstat((int)fd, &st) == 0 || file_fail(err);
}

#else

bool pal_fchmod(int64_t fd, uint32_t mode, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!fd_ok(fd, err))
        return false;
    for (;;) {
        if (fchmod((int)fd, (mode_t)(mode & 07777)) == 0)
            return true;
        if (errno != EINTR)
            return file_fail(err);
    }
}

#endif

#if defined(BURROW_OS_WASI)

bool pal_fchown(int64_t fd, int64_t uid, int64_t gid, PalErrno *err) {
    (void)fd;
    (void)uid;
    (void)gid;
    return file_nosys(err);
}

#else

bool pal_fchown(int64_t fd, int64_t uid, int64_t gid, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!fd_ok(fd, err))
        return false;
    uid_t u = uid < 0 ? (uid_t)-1 : (uid_t)uid;
    gid_t g = gid < 0 ? (gid_t)-1 : (gid_t)gid;
    for (;;) {
        if (fchown((int)fd, u, g) == 0)
            return true;
        if (errno != EINTR)
            return file_fail(err);
    }
}

#endif

static struct timespec timespec_from_ns(int64_t ns) {
    int64_t sec = ns / 1000000000;
    int64_t nsec = ns % 1000000000;
    if (nsec < 0) {
        sec--;
        nsec += 1000000000;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)sec;
    ts.tv_nsec = (long)nsec;
    return ts;
}

bool pal_truncate(const char *path, int64_t size, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(path, err))
        return false;
    if (size < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    for (;;) {
        if (truncate(path, (off_t)size) == 0)
            return true;
        if (errno != EINTR)
            return file_fail(err);
    }
}

bool pal_utimes(const char *path, int64_t atime_ns, int64_t mtime_ns, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(path, err))
        return false;
    struct timespec ts[2] = {timespec_from_ns(atime_ns), timespec_from_ns(mtime_ns)};
    if (atime_ns == PAL_UTIME_OMIT)
        ts[0] = (struct timespec){0, UTIME_OMIT};
    if (mtime_ns == PAL_UTIME_OMIT)
        ts[1] = (struct timespec){0, UTIME_OMIT};
    return utimensat(AT_FDCWD, path, ts, 0) == 0 || file_fail(err);
}

/* ------------------------------------------------------------- the at calls */

/* A directory descriptor as the at calls take it. */
static bool dirfd_ok(int64_t fd, PalErrno *err) {
    return fd_ok(fd, err);
}

int64_t pal_openat(int64_t dirfd, const char *path, uint32_t flags, uint32_t mode,
                   PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!dirfd_ok(dirfd, err) || !posix_path_ok(path, err))
        return PAL_INVALID_HANDLE;
    int o = open_flags(flags);
    for (;;) {
        int fd = openat((int)dirfd, path, o, (mode_t)(mode & 07777));
        if (fd >= 0)
            return fd;
        if (errno != EINTR)
            return file_fail_n(err);
    }
}

bool pal_lstatat(int64_t dirfd, const char *path, PalStat *out, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!dirfd_ok(dirfd, err) || !posix_path_ok(path, err))
        return false;
    if (out == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    struct stat st;
    for (;;) {
        if (fstatat((int)dirfd, path, &st, AT_SYMLINK_NOFOLLOW) == 0)
            break;
        if (errno != EINTR)
            return file_fail(err);
    }
    stat_from(&st, out);
    return true;
}

bool pal_mkdirat(int64_t dirfd, const char *path, uint32_t mode, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!dirfd_ok(dirfd, err) || !posix_path_ok(path, err))
        return false;
    for (;;) {
        if (mkdirat((int)dirfd, path, (mode_t)(mode & 07777)) == 0)
            return true;
        if (errno != EINTR)
            return file_fail(err);
    }
}

bool pal_unlinkat(int64_t dirfd, const char *path, bool dir, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!dirfd_ok(dirfd, err) || !posix_path_ok(path, err))
        return false;
    for (;;) {
        if (unlinkat((int)dirfd, path, dir ? AT_REMOVEDIR : 0) == 0)
            return true;
        if (errno != EINTR)
            return file_fail(err);
    }
}

int64_t pal_readlinkat(int64_t dirfd, const char *path, char *buf, int64_t cap,
                       PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!dirfd_ok(dirfd, err) || !posix_path_ok(path, err) || !count_ok(buf, cap, err))
        return -1;
    ssize_t n;
    for (;;) {
        n = readlinkat((int)dirfd, path, buf, (size_t)cap);
        if (n >= 0)
            break;
        if (errno != EINTR)
            return file_fail_n(err);
    }
    /* As for pal_readlink, a full buffer may be a truncated target. */
    if ((int64_t)n >= cap) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    return (int64_t)n;
}

bool pal_renameat(int64_t olddirfd, const char *from, int64_t newdirfd, const char *to,
                  PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!dirfd_ok(olddirfd, err) || !dirfd_ok(newdirfd, err) ||
        !posix_path_ok(from, err) || !posix_path_ok(to, err))
        return false;
    return renameat((int)olddirfd, from, (int)newdirfd, to) == 0 || file_fail(err);
}

bool pal_linkat(int64_t olddirfd, const char *from, int64_t newdirfd, const char *to,
                PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!dirfd_ok(olddirfd, err) || !dirfd_ok(newdirfd, err) ||
        !posix_path_ok(from, err) || !posix_path_ok(to, err))
        return false;
    return linkat((int)olddirfd, from, (int)newdirfd, to, 0) == 0 || file_fail(err);
}

bool pal_symlinkat(const char *target, int64_t dirfd, const char *path, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!posix_path_ok(target, err) || !dirfd_ok(dirfd, err) ||
        !posix_path_ok(path, err))
        return false;
    return symlinkat(target, (int)dirfd, path) == 0 || file_fail(err);
}

#if defined(BURROW_OS_WASI)

/* Go's Fchmodat on wasip1 is ENOSYS, unlike its Chmod. */
bool pal_fchmodat(int64_t dirfd, const char *path, uint32_t mode, PalErrno *err) {
    (void)dirfd;
    (void)path;
    (void)mode;
    return file_nosys(err);
}

#else

bool pal_fchmodat(int64_t dirfd, const char *path, uint32_t mode, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!dirfd_ok(dirfd, err) || !posix_path_ok(path, err))
        return false;
    /* The Linux system call ignores AT_SYMLINK_NOFOLLOW. glibc and musl go
     * through fchmodat2, or an O_PATH descriptor on an older kernel, the same
     * way Go does. */
    for (;;) {
        if (fchmodat((int)dirfd, path, (mode_t)(mode & 07777), AT_SYMLINK_NOFOLLOW) ==
            0)
            return true;
        if (errno != EINTR)
            return file_fail(err);
    }
}

#endif

#if defined(BURROW_OS_WASI)

bool pal_fchownat(int64_t dirfd, const char *path, int64_t uid, int64_t gid,
                  PalErrno *err) {
    (void)dirfd;
    (void)path;
    (void)uid;
    (void)gid;
    return file_nosys(err);
}

#else

bool pal_fchownat(int64_t dirfd, const char *path, int64_t uid, int64_t gid,
                  PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!dirfd_ok(dirfd, err) || !posix_path_ok(path, err))
        return false;
    uid_t u = uid < 0 ? (uid_t)-1 : (uid_t)uid;
    gid_t g = gid < 0 ? (gid_t)-1 : (gid_t)gid;
    for (;;) {
        if (fchownat((int)dirfd, path, u, g, AT_SYMLINK_NOFOLLOW) == 0)
            return true;
        if (errno != EINTR)
            return file_fail(err);
    }
}

#endif

bool pal_utimesat(int64_t dirfd, const char *path, int64_t atime_ns, int64_t mtime_ns,
                  PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!dirfd_ok(dirfd, err) || !posix_path_ok(path, err))
        return false;
    struct timespec ts[2] = {timespec_from_ns(atime_ns), timespec_from_ns(mtime_ns)};
    if (atime_ns == PAL_UTIME_OMIT)
        ts[0] = (struct timespec){0, UTIME_OMIT};
    if (mtime_ns == PAL_UTIME_OMIT)
        ts[1] = (struct timespec){0, UTIME_OMIT};
    for (;;) {
        if (utimensat((int)dirfd, path, ts, AT_SYMLINK_NOFOLLOW) == 0)
            return true;
        if (errno != EINTR)
            return file_fail(err);
    }
}

int64_t pal_dup(int64_t fd, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!fd_ok(fd, err))
        return -1;
    int nfd = fcntl((int)fd, F_DUPFD_CLOEXEC, 0);
    if (nfd < 0)
        return file_fail_n(err);
    return nfd;
}

#if !(defined(BURROW_OS_LINUX) || defined(BURROW_OS_FREEBSD) ||                        \
      defined(BURROW_OS_NETBSD) || defined(BURROW_OS_OPENBSD) ||                       \
      defined(BURROW_OS_DRAGONFLY) || defined(BURROW_OS_WASI))
static bool set_fd_flag(int fd, int get, int set, int flag) {
    int cur = fcntl(fd, get);
    return cur >= 0 && fcntl(fd, set, cur | flag) == 0;
}

#endif

bool pal_pipe(int64_t out[2], uint32_t flags, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (out == NULL || (flags & ~(uint32_t)PAL_O_NONBLOCK) != 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    int p[2];
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_FREEBSD) ||                          \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_OPENBSD) ||                         \
    defined(BURROW_OS_DRAGONFLY)
    int o = O_CLOEXEC | ((flags & PAL_O_NONBLOCK) ? O_NONBLOCK : 0);
    if (pipe2(p, o) != 0)
        return file_fail(err);
#elif defined(BURROW_OS_WASI)
    /* wasip1 has no pipes, and Go's Pipe there is ENOSYS. */
    (void)p;
    return file_nosys(err);
#else
    /* No pipe2, so there is a moment where both ends are open without close on
     * exec, and a fork on another thread in that moment would give the child a
     * copy. Go closes it with a lock that every fork takes, and so does
     * pal_spawn. */
    burrow__pal_fork_rlock();
    if (pipe(p) != 0) {
        PalErrno e = burrow__pal_errno(errno);
        burrow__pal_fork_runlock();
        BURROW_OUT(err, e);
        return false;
    }
    for (int i = 0; i < 2; i++) {
        bool ok = set_fd_flag(p[i], F_GETFD, F_SETFD, FD_CLOEXEC);
        if (ok && (flags & PAL_O_NONBLOCK))
            ok = set_fd_flag(p[i], F_GETFL, F_SETFL, (int)O_NONBLOCK);
        if (!ok) {
            PalErrno e = burrow__pal_errno(errno);
            close(p[0]);
            close(p[1]);
            burrow__pal_fork_runlock();
            BURROW_OUT(err, e);
            return false;
        }
    }
    burrow__pal_fork_runlock();
#endif
    out[0] = p[0];
    out[1] = p[1];
    return true;
}

#endif /* !BURROW_OS_WINDOWS */

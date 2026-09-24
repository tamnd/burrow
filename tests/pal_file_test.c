/* The file group of the platform layer.
 *
 * Every test works in a directory of its own under the system's temporary
 * directory, made with pal_mkdir and taken apart afterwards with pal_readdir,
 * pal_unlink and pal_rmdir, so the cleanup is a test of those three as well.
 * What is checked is what burrow/pal.h promises and what the os package will
 * build on: the flags mean the same thing everywhere, a failure comes back as
 * the same PalErrno on every system, and a directory listing has every name in
 * it once and no "." or "..".
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/testing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------- the scratch dir */

typedef struct Scratch {
    char dir[1024];
    char path[1100];
} Scratch;

static void remove_tree(const char *dir);

static void remove_entry(const char *dir, const PalDirEntry *e) {
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", dir, e->name);
    PalStat st;
    if (pal_lstat(p, &st, NULL) && (st.mode & PAL_S_IFMT) == PAL_S_IFDIR)
        remove_tree(p);
    else if (!pal_unlink(p, NULL))
        pal_rmdir(p, NULL); /* a directory symlink on Windows */
}

static void remove_tree(const char *dir) {
    int64_t fd = pal_open(dir, PAL_O_RDONLY | PAL_O_DIRECTORY, 0, NULL);
    if (fd >= 0) {
        static PalDir d;
        PalDirEntry e;
        /* Collect first, then remove, because removing while reading is
         * allowed to skip names on every platform. */
        char names[64][PAL_NAME_MAX + 1];
        int n = 0;
        d = (PalDir){.fd = fd};
        while (n < 64 && pal_readdir(&d, &e, NULL))
            memcpy(names[n++], e.name, (size_t)e.name_len + 1);
        pal_close(fd, NULL);
        for (int i = 0; i < n; i++) {
            PalDirEntry one = {0};
            memcpy(one.name, names[i], strlen(names[i]) + 1);
            remove_entry(dir, &one);
        }
    }
    pal_rmdir(dir, NULL);
}

static void scratch_free(void *env) {
    Scratch *s = env;
    remove_tree(s->dir);
    mem_free(heap_allocator(), s, sizeof *s, _Alignof(Scratch));
}

static const char *temp_root(void) {
    const char *names[] = {"TMPDIR", "TEMP", "TMP"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *v = getenv(names[i]);
        if (v != NULL && v[0] != '\0')
            return v;
    }
    return "/tmp";
}

static Scratch *scratch(TestingT *t) {
    Scratch *s = mem_alloc(heap_allocator(), sizeof *s, _Alignof(Scratch));
    if (s == NULL)
        testing_t_fatal_v(t, "out of memory");
    uint32_t r = 0;
    pal_random_bytes(&r, sizeof r, NULL);
    snprintf(s->dir, sizeof s->dir, "%s/burrow-pal-%08x", temp_root(), (unsigned)r);
    PalErrno err = PAL_OK;
    if (!pal_mkdir(s->dir, 0700, &err))
        testing_t_fatalf_v(t, "mkdir %s: %s", s->dir, pal_errno_string(err));
    testing_t_cleanup(t, BURROW_FN(Func, scratch_free, s));
    return s;
}

static const char *at(Scratch *s, const char *name) {
    snprintf(s->path, sizeof s->path, "%s/%s", s->dir, name);
    return s->path;
}

static void write_file(TestingT *t, const char *path, const char *text) {
    PalErrno err = PAL_OK;
    int64_t fd = pal_open(path, PAL_O_WRONLY | PAL_O_CREATE | PAL_O_TRUNC, 0644, &err);
    if (fd < 0)
        testing_t_fatalf_v(t, "open %s: %s", path, pal_errno_string(err));
    int64_t n = (int64_t)strlen(text);
    if (pal_write(fd, text, n, &err) != n)
        testing_t_fatalf_v(t, "write %s: %s", path, pal_errno_string(err));
    pal_close(fd, NULL);
}

/* ------------------------------------------------------------------- tests */

static void TestWriteThenReadBack(TestingT *t) {
    Scratch *s = scratch(t);
    const char *p = at(s, "a.txt");
    write_file(t, p, "hello, world");

    PalErrno err = PAL_OK;
    int64_t fd = pal_open(p, PAL_O_RDONLY, 0, &err);
    if (fd < 0)
        testing_t_fatalf_v(t, "open: %s", pal_errno_string(err));
    char buf[64] = {0};
    int64_t n = pal_read(fd, buf, sizeof buf, &err);
    if (n != 12 || memcmp(buf, "hello, world", 12) != 0)
        testing_t_errorf_v(t, "read %d bytes %q, want 12 and %q", n, buf,
                           "hello, world");
    if (pal_read(fd, buf, sizeof buf, &err) != 0 || err != PAL_OK)
        testing_t_errorf_v(t, "read at the end did not give 0: %s",
                           pal_errno_string(err));
    pal_close(fd, NULL);
}

static void TestSeekAndPositionalIO(TestingT *t) {
    Scratch *s = scratch(t);
    const char *p = at(s, "b.txt");
    write_file(t, p, "0123456789");

    PalErrno err = PAL_OK;
    int64_t fd = pal_open(p, PAL_O_RDWR, 0, &err);
    if (fd < 0)
        testing_t_fatalf_v(t, "open: %s", pal_errno_string(err));

    char buf[4] = {0};
    if (pal_pread(fd, buf, 3, 4, &err) != 3 || memcmp(buf, "456", 3) != 0)
        testing_t_errorf_v(t, "pread at 4 = %q, want %q", buf, "456");
    /* A positional read does not move the offset. */
    if (pal_seek(fd, 0, PAL_SEEK_CUR, &err) != 0)
        testing_t_errorf_v(t, "offset after pread = %d, want 0",
                           pal_seek(fd, 0, PAL_SEEK_CUR, NULL));
    if (pal_pwrite(fd, "xy", 2, 8, &err) != 2)
        testing_t_errorf_v(t, "pwrite: %s", pal_errno_string(err));
    if (pal_seek(fd, -3, PAL_SEEK_END, &err) != 7)
        testing_t_errorf_v(t, "seek -3 from the end did not land on 7: %s",
                           pal_errno_string(err));
    if (pal_read(fd, buf, 3, &err) != 3 || memcmp(buf, "7xy", 3) != 0)
        testing_t_errorf_v(t, "read after seek = %q, want %q", buf, "7xy");
    if (pal_seek(fd, 0, 99, &err) != -1 || err != PAL_EINVAL)
        testing_t_errorf_v(t, "a bad whence gave %s, want EINVAL",
                           pal_errno_string(err));
    pal_close(fd, NULL);
}

static void TestAppendWritesAtTheEnd(TestingT *t) {
    Scratch *s = scratch(t);
    const char *p = at(s, "c.txt");
    write_file(t, p, "abc");

    PalErrno err = PAL_OK;
    int64_t fd = pal_open(p, PAL_O_WRONLY | PAL_O_APPEND, 0, &err);
    if (fd < 0)
        testing_t_fatalf_v(t, "open: %s", pal_errno_string(err));
    pal_write(fd, "def", 3, &err);
    pal_close(fd, NULL);

    PalStat st;
    if (!pal_stat(p, &st, &err))
        testing_t_fatalf_v(t, "stat: %s", pal_errno_string(err));
    if (st.size != 6)
        testing_t_errorf_v(t, "size after append = %d, want 6", st.size);
}

static void TestCreateExclusiveRefusesAnExistingFile(TestingT *t) {
    Scratch *s = scratch(t);
    const char *p = at(s, "d.txt");
    write_file(t, p, "x");

    PalErrno err = PAL_OK;
    int64_t fd = pal_open(p, PAL_O_WRONLY | PAL_O_CREATE | PAL_O_EXCL, 0644, &err);
    if (fd >= 0 || err != PAL_EEXIST)
        testing_t_errorf_v(t, "exclusive create of an existing file gave %d and %s", fd,
                           pal_errno_string(err));
    if (fd >= 0)
        pal_close(fd, NULL);
}

static void TestTruncateEmptiesTheFile(TestingT *t) {
    Scratch *s = scratch(t);
    const char *p = at(s, "e.txt");
    write_file(t, p, "a longer line of text");

    PalErrno err = PAL_OK;
    int64_t fd = pal_open(p, PAL_O_RDWR, 0, &err);
    if (!pal_ftruncate(fd, 4, &err))
        testing_t_errorf_v(t, "ftruncate: %s", pal_errno_string(err));
    if (!pal_fsync(fd, &err))
        testing_t_errorf_v(t, "fsync: %s", pal_errno_string(err));
    PalStat st;
    if (!pal_fstat(fd, &st, &err) || st.size != 4)
        testing_t_errorf_v(t, "size after ftruncate = %d, want 4", st.size);
    pal_close(fd, NULL);

    fd = pal_open(p, PAL_O_WRONLY | PAL_O_TRUNC, 0, &err);
    pal_close(fd, NULL);
    if (!pal_stat(p, &st, &err) || st.size != 0)
        testing_t_errorf_v(t, "size after O_TRUNC = %d, want 0", st.size);
}

static void TestMissingFilesAreENOENT(TestingT *t) {
    Scratch *s = scratch(t);
    PalErrno err = PAL_OK;
    PalStat st;

    if (pal_open(at(s, "nope"), PAL_O_RDONLY, 0, &err) != -1 || err != PAL_ENOENT)
        testing_t_errorf_v(t, "open gave %s, want ENOENT", pal_errno_string(err));
    err = PAL_OK;
    if (pal_stat(at(s, "nope"), &st, &err) || err != PAL_ENOENT)
        testing_t_errorf_v(t, "stat gave %s, want ENOENT", pal_errno_string(err));
    err = PAL_OK;
    if (pal_unlink(at(s, "nope"), &err) || err != PAL_ENOENT)
        testing_t_errorf_v(t, "unlink gave %s, want ENOENT", pal_errno_string(err));
    err = PAL_OK;
    if (pal_rmdir(at(s, "nope"), &err) || err != PAL_ENOENT)
        testing_t_errorf_v(t, "rmdir gave %s, want ENOENT", pal_errno_string(err));
    err = PAL_OK;
    if (pal_open(at(s, "nope/deeper"), PAL_O_RDONLY, 0, &err) != -1 ||
        err != PAL_ENOENT)
        testing_t_errorf_v(t, "open under a missing dir gave %s, want ENOENT",
                           pal_errno_string(err));
}

static void TestStatTellsFilesFromDirectories(TestingT *t) {
    Scratch *s = scratch(t);
    write_file(t, at(s, "f"), "12345");
    PalErrno err = PAL_OK;
    if (!pal_mkdir(at(s, "sub"), 0755, &err))
        testing_t_fatalf_v(t, "mkdir: %s", pal_errno_string(err));

    PalStat st;
    pal_stat(at(s, "f"), &st, &err);
    if ((st.mode & PAL_S_IFMT) != PAL_S_IFREG || st.size != 5)
        testing_t_errorf_v(t, "file: mode %o size %d", st.mode, st.size);
    if (st.mtime_sec < 1767225600) /* 2026 */
        testing_t_errorf_v(t, "file: mtime %d is in the past", st.mtime_sec);
    if (st.nlink < 1)
        testing_t_errorf_v(t, "file: nlink %d", st.nlink);

    pal_stat(at(s, "sub"), &st, &err);
    if ((st.mode & PAL_S_IFMT) != PAL_S_IFDIR)
        testing_t_errorf_v(t, "dir: mode %o, want a directory", st.mode);

    err = PAL_OK;
    if (pal_mkdir(at(s, "sub"), 0755, &err) || err != PAL_EEXIST)
        testing_t_errorf_v(t, "second mkdir gave %s, want EEXIST",
                           pal_errno_string(err));
}

static void TestReadDirListsEveryNameOnce(TestingT *t) {
    Scratch *s = scratch(t);
    const char *want[] = {"alpha", "beta", "gamma.txt", "dir", "héllo"};
    for (int i = 0; i < 5; i++) {
        if (i == 3)
            pal_mkdir(at(s, want[i]), 0755, NULL);
        else
            write_file(t, at(s, want[i]), want[i]);
    }

    PalErrno err = PAL_OK;
    int64_t fd = pal_open(s->dir, PAL_O_RDONLY | PAL_O_DIRECTORY, 0, &err);
    if (fd < 0)
        testing_t_fatalf_v(t, "open dir: %s", pal_errno_string(err));

    static PalDir d;
    d = (PalDir){.fd = fd};
    PalDirEntry e;
    int seen[5] = {0};
    int total = 0;
    while (pal_readdir(&d, &e, &err)) {
        total++;
        if ((size_t)e.name_len != strlen(e.name))
            testing_t_errorf_v(t, "%s: name_len %d", e.name, e.name_len);
        int found = -1;
        for (int i = 0; i < 5; i++)
            if (strcmp(e.name, want[i]) == 0)
                found = i;
        if (found < 0) {
            testing_t_errorf_v(t, "unexpected entry %q", e.name);
            continue;
        }
        seen[found]++;
        uint32_t type = found == 3 ? PAL_S_IFDIR : PAL_S_IFREG;
        if (e.type != 0 && e.type != type)
            testing_t_errorf_v(t, "%s: type %o, want %o", e.name, e.type, type);
    }
    if (err == PAL_ENOTSUP && total == 0) {
        pal_close(fd, NULL);
        testing_t_skip_v(t, "no pal_readdir backend on this platform yet");
    }
    if (err != PAL_OK)
        testing_t_errorf_v(t, "readdir: %s", pal_errno_string(err));
    for (int i = 0; i < 5; i++)
        if (seen[i] != 1)
            testing_t_errorf_v(t, "%s seen %d times", want[i], seen[i]);
    if (total != 5)
        testing_t_errorf_v(t, "%d entries, want 5", total);

    /* The end stays the end. */
    if (pal_readdir(&d, &e, &err) || err != PAL_OK)
        testing_t_errorf_v(t, "readdir after the end: %s", pal_errno_string(err));
    pal_close(fd, NULL);
}

static void TestReadDirOfAFileFails(TestingT *t) {
    Scratch *s = scratch(t);
    write_file(t, at(s, "plain"), "x");
    PalErrno err = PAL_OK;
    int64_t fd = pal_open(at(s, "plain"), PAL_O_RDONLY | PAL_O_DIRECTORY, 0, &err);
    if (fd >= 0 || err != PAL_ENOTDIR)
        testing_t_errorf_v(t, "opening a file as a directory gave %s, want ENOTDIR",
                           pal_errno_string(err));
    if (fd >= 0)
        pal_close(fd, NULL);
}

static void TestRenameReplacesTheTarget(TestingT *t) {
    Scratch *s = scratch(t);
    char from[1100];
    snprintf(from, sizeof from, "%s", at(s, "from"));
    write_file(t, from, "new");
    write_file(t, at(s, "to"), "old contents");

    PalErrno err = PAL_OK;
    if (!pal_rename(from, at(s, "to"), &err))
        testing_t_fatalf_v(t, "rename: %s", pal_errno_string(err));
    PalStat st;
    if (!pal_stat(at(s, "to"), &st, &err) || st.size != 3)
        testing_t_errorf_v(t, "target size %d, want 3", st.size);
    err = PAL_OK;
    if (pal_stat(from, &st, &err) || err != PAL_ENOENT)
        testing_t_errorf_v(t, "the old name is still there: %s", pal_errno_string(err));
}

static void TestRemovingADirectoryWithSomethingInItFails(TestingT *t) {
    Scratch *s = scratch(t);
    pal_mkdir(at(s, "full"), 0755, NULL);
    write_file(t, at(s, "full/x"), "x");
    PalErrno err = PAL_OK;
    if (pal_rmdir(at(s, "full"), &err))
        testing_t_errorf_v(t, "rmdir of a full directory worked");
    else if (err != PAL_ENOTEMPTY && err != PAL_EEXIST)
        testing_t_errorf_v(t, "rmdir of a full directory gave %s, want ENOTEMPTY",
                           pal_errno_string(err));
}

static void TestLinksAndSymlinks(TestingT *t) {
    Scratch *s = scratch(t);
    write_file(t, at(s, "target"), "data");

    PalErrno err = PAL_OK;
    char link[1100];
    snprintf(link, sizeof link, "%s", at(s, "hard"));
    if (!pal_link(at(s, "target"), link, &err))
        testing_t_fatalf_v(t, "link: %s", pal_errno_string(err));
    PalStat st;
    if (!pal_stat(link, &st, &err) || st.nlink != 2)
        testing_t_errorf_v(t, "nlink after link = %d, want 2", st.nlink);

    snprintf(link, sizeof link, "%s", at(s, "soft"));
    if (!pal_symlink("target", link, &err)) {
        /* Windows without Developer Mode or elevation cannot make one. */
        if (err == PAL_EPERM || err == PAL_EACCES || err == PAL_ENOTSUP)
            testing_t_skipf_v(t, "symlink: %s", pal_errno_string(err));
        testing_t_fatalf_v(t, "symlink: %s", pal_errno_string(err));
    }
    if (!pal_lstat(link, &st, &err) || (st.mode & PAL_S_IFMT) != PAL_S_IFLNK)
        testing_t_errorf_v(t, "lstat of a symlink: mode %o", st.mode);
    if (!pal_stat(link, &st, &err) || (st.mode & PAL_S_IFMT) != PAL_S_IFREG ||
        st.size != 4)
        testing_t_errorf_v(t, "stat through a symlink: mode %o size %d", st.mode,
                           st.size);

    char buf[64];
    int64_t n = pal_readlink(link, buf, sizeof buf, &err);
    if (n != 6 || memcmp(buf, "target", 6) != 0)
        testing_t_errorf_v(t, "readlink gave %d bytes: %s", n, pal_errno_string(err));
    err = PAL_OK;
    if (pal_readlink(link, buf, 3, &err) != -1 || err != PAL_ERANGE)
        testing_t_errorf_v(t, "readlink into a short buffer gave %s, want ERANGE",
                           pal_errno_string(err));
}

static void TestChmodAndUtimes(TestingT *t) {
    Scratch *s = scratch(t);
    const char *p = at(s, "m");
    write_file(t, p, "x");

    PalErrno err = PAL_OK;
    PalStat st;
    if (!pal_chmod(p, 0444, &err))
        testing_t_fatalf_v(t, "chmod: %s", pal_errno_string(err));
    pal_stat(p, &st, &err);
    if ((st.mode & 0777) != 0444)
        testing_t_errorf_v(t, "mode after chmod 0444 = %o", st.mode & 0777);
    pal_chmod(p, 0644, &err);
    pal_stat(p, &st, &err);
    if ((st.mode & 0200) == 0)
        testing_t_errorf_v(t, "mode after chmod 0644 = %o", st.mode & 0777);

    int64_t when = (int64_t)1700000000 * 1000000000 + 5000000;
    if (!pal_utimes(p, when, when, &err))
        testing_t_fatalf_v(t, "utimes: %s", pal_errno_string(err));
    pal_stat(p, &st, &err);
    if (st.mtime_sec != 1700000000 || st.mtime_nsec / 1000000 != 5)
        testing_t_errorf_v(t, "mtime after utimes = %d.%09d", st.mtime_sec,
                           st.mtime_nsec);
}

static void TestPipeAndDup(TestingT *t) {
    int64_t p[2];
    PalErrno err = PAL_OK;
    if (!pal_pipe(p, 0, &err))
        testing_t_fatalf_v(t, "pipe: %s", pal_errno_string(err));
    int64_t w = pal_dup(p[1], &err);
    if (w < 0)
        testing_t_fatalf_v(t, "dup: %s", pal_errno_string(err));
    pal_close(p[1], NULL);
    if (pal_write(w, "ping", 4, &err) != 4)
        testing_t_errorf_v(t, "write to the dup: %s", pal_errno_string(err));
    pal_close(w, NULL);

    char buf[8] = {0};
    if (pal_read(p[0], buf, sizeof buf, &err) != 4 || memcmp(buf, "ping", 4) != 0)
        testing_t_errorf_v(t, "read %q from the pipe", buf);
    /* Every write end is closed, so the next read is the end of the file. */
    if (pal_read(p[0], buf, sizeof buf, &err) != 0)
        testing_t_errorf_v(t, "read after the writers closed: %s",
                           pal_errno_string(err));
    pal_close(p[0], NULL);

    PalStat st;
    if (pal_fstat(w, &st, &err) || err != PAL_EBADF)
        testing_t_errorf_v(t, "fstat of a closed descriptor gave %s, want EBADF",
                           pal_errno_string(err));
}

static void TestChdirAndGetcwd(TestingT *t) {
    Scratch *s = scratch(t);
    char home[1024];
    PalErrno err = PAL_OK;
    int64_t n = pal_getcwd(home, sizeof home, &err);
    if (n <= 0 || (size_t)n != strlen(home))
        testing_t_fatalf_v(t, "getcwd: %d, %s", n, pal_errno_string(err));
    char tiny[2];
    if (pal_getcwd(tiny, sizeof tiny, &err) != -1 || err != PAL_ERANGE)
        testing_t_errorf_v(t, "getcwd into two bytes: %s, want ERANGE",
                           pal_errno_string(err));

    char sub[1100];
    snprintf(sub, sizeof sub, "%s", at(s, "sub"));
    if (!pal_mkdir(sub, 0755, &err))
        testing_t_fatalf_v(t, "mkdir: %s", pal_errno_string(err));
    write_file(t, at(s, "sub/here"), "x");
    if (!pal_chdir(sub, &err))
        testing_t_fatalf_v(t, "chdir %s: %s", sub, pal_errno_string(err));
    PalStat st;
    if (!pal_stat("here", &st, &err))
        testing_t_errorf_v(t, "a relative name after chdir: %s", pal_errno_string(err));
    char now[1024];
    n = pal_getcwd(now, sizeof now, &err);
    if (n < 4 ||
        (strcmp(now + n - 4, "/sub") != 0 && strcmp(now + n - 4, "\\sub") != 0))
        testing_t_errorf_v(t, "getcwd after chdir is %s", now);
    if (pal_chdir("missing", &err) || err != PAL_ENOENT)
        testing_t_errorf_v(t, "chdir to a missing name: %s, want ENOENT",
                           pal_errno_string(err));
    if (!pal_chdir(home, &err))
        testing_t_fatalf_v(t, "chdir back to %s: %s", home, pal_errno_string(err));
}

static void TestNullArgumentsAreRefused(TestingT *t) {
    PalErrno err = PAL_OK;
    if (pal_open(NULL, PAL_O_RDONLY, 0, &err) != -1 || err != PAL_EINVAL)
        testing_t_errorf_v(t, "open(NULL) gave %s", pal_errno_string(err));
    err = PAL_OK;
    if (pal_readdir(NULL, NULL, &err) || err != PAL_EINVAL)
        testing_t_errorf_v(t, "readdir(NULL) gave %s", pal_errno_string(err));
    err = PAL_OK;
    if (pal_read(-1, NULL, 0, &err) != -1 || err != PAL_EBADF)
        testing_t_errorf_v(t, "read(-1) gave %s", pal_errno_string(err));
    /* The error is optional here as everywhere. */
    if (pal_stat(NULL, NULL, NULL))
        testing_t_errorf_v(t, "stat(NULL) worked");
}

#define TESTS(X)                                                                       \
    X(TestWriteThenReadBack)                                                           \
    X(TestSeekAndPositionalIO)                                                         \
    X(TestAppendWritesAtTheEnd)                                                        \
    X(TestCreateExclusiveRefusesAnExistingFile)                                        \
    X(TestTruncateEmptiesTheFile)                                                      \
    X(TestMissingFilesAreENOENT)                                                       \
    X(TestStatTellsFilesFromDirectories)                                               \
    X(TestReadDirListsEveryNameOnce)                                                   \
    X(TestReadDirOfAFileFails)                                                         \
    X(TestRenameReplacesTheTarget)                                                     \
    X(TestRemovingADirectoryWithSomethingInItFails)                                    \
    X(TestLinksAndSymlinks)                                                            \
    X(TestChmodAndUtimes)                                                              \
    X(TestPipeAndDup)                                                                  \
    X(TestChdirAndGetcwd)                                                              \
    X(TestNullArgumentsAreRefused)

TESTING_MAIN(TESTS)

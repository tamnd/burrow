/* What the machine is, on everything that is not Windows.
 *
 * Two questions. The page size cannot change while the process runs, so it is
 * asked once and kept. The cache is a plain relaxed atomic and not a lock: two
 * threads arriving together both ask the system, both get the same number, and
 * both write it. There is nothing to publish alongside it, so there is nothing
 * for a release store to order.
 *
 * The processor count is asked every time, because it is the affinity mask and
 * a process can be moved onto fewer processors while it runs. sched_setaffinity
 * does it, a container being resized does it, and a batch scheduler does it.
 * Nobody asks this in a loop, so what a cache would save is a system call at
 * startup and what it would cost is an answer that quietly goes stale.
 *
 * _GNU_SOURCE is here for sched_getaffinity and CPU_COUNT, and it goes before
 * the first include because glibc reads the feature test macros when its first
 * header arrives. On a platform that is not glibc it is ignored and the sysconf
 * path below is the one that runs.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if defined(__linux__)
#define _GNU_SOURCE 1
#endif

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sched.h>
#include <sys/utsname.h>
#endif

/* Zero until somebody has asked. A page size of zero is impossible, so zero can
 * be the "not yet" marker without a second flag. */
static uint32_t cached_page;

int64_t pal_page_size(void) {
    uint32_t got = burrow__atomic_load_relaxed_u32(&cached_page);
    if (got != 0)
        return (int64_t)got;

    long n = sysconf(_SC_PAGESIZE);

    /* 4096 when the system will not say, which is right nearly everywhere and
     * is the smallest granularity that is safe to assume when it is wrong. A
     * value that is not a power of two is not a page size and is treated as no
     * answer, because everything above here rounds with a mask. */
    if (n <= 0 || (n & (n - 1)) != 0)
        n = 4096;

    burrow__atomic_store_relaxed_u32(&cached_page, (uint32_t)n);
    return (int64_t)n;
}

int64_t pal_cpu_count(void) {
    long n = 0;

#if defined(__linux__)
    /* The affinity mask rather than the machine, so a process pinned to two
     * cores sees two and not the ninety six on the host. This is what Go does
     * and it is why NumCPU is right inside a cpuset.
     *
     * It is deliberately not the CFS quota. A container run with --cpus=0.5 has
     * every core in its mask and a budget it gets throttled against, and a
     * runtime that sized itself from the quota would run one thread on a
     * sixteen core machine because somebody asked for half a core of average
     * use. Go made the same call.
     *
     * The mask is asked for at a size that covers a thousand and twenty four
     * processors, which is the kernel's own default maximum. A machine with
     * more of them than that answers EINVAL and falls through to sysconf. */
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0)
        n = CPU_COUNT(&set);
#endif

#if defined(BURROW_OS_WASI)
    /* One thread is all wasip1 has, and Go's NumCPU there is 1. */
    n = 1;
#endif

    if (n <= 0)
        n = sysconf(_SC_NPROCESSORS_ONLN);

    /* Never less than one, including on a platform that will not say. A zero
     * here would size the scheduler to nothing. */
    if (n <= 0)
        n = 1;

    return (int64_t)n;
}

/* Go's hostname: uname on Linux, with /proc for a name uname may have cut
 * short, and kern.hostname everywhere else, which is what gethostname asks. */
int64_t pal_hostname(char *buf, int64_t cap, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (buf == NULL || cap <= 0) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    char tmp[1025];
    size_t n = 0;
#if defined(__linux__)
    struct utsname un;
    if (uname(&un) == 0) {
        n = strnlen(un.nodename, sizeof un.nodename);
        memcpy(tmp, un.nodename, n);
    }
    /* The node name is 65 bytes, so 64 or more may be the front of a longer
     * one. */
    if (n == 0 || n >= 64) {
        int fd;
        do
            fd = open("/proc/sys/kernel/hostname", O_RDONLY | O_CLOEXEC);
        while (fd < 0 && errno == EINTR);
        if (fd < 0) {
            BURROW_OUT(err, burrow__pal_errno(errno));
            return -1;
        }
        ssize_t r;
        do
            r = read(fd, tmp, 512);
        while (r < 0 && errno == EINTR);
        int why = errno;
        close(fd);
        if (r < 0) {
            BURROW_OUT(err, burrow__pal_errno(why));
            return -1;
        }
        n = (size_t)r;
        if (n > 0 && tmp[n - 1] == '\n')
            n--;
    }
#elif defined(BURROW_OS_WASI)
    /* WASI has no call that names the host. Go's os.Hostname on wasip1 asks
     * syscall.Sysctl("kern.hostname"), and that answers this. */
    n = sizeof "wasip1" - 1;
    memcpy(tmp, "wasip1", n);
#else
    if (gethostname(tmp, sizeof tmp - 1) != 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return -1;
    }
    tmp[sizeof tmp - 1] = 0;
    n = strlen(tmp);
#endif
    if ((int64_t)n >= cap) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    memcpy(buf, tmp, n);
    buf[n] = 0;
    return (int64_t)n;
}

int64_t pal_hosts_path(char *buf, int64_t cap, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    static const char path[] = "/etc/hosts";
    if (buf == NULL || cap < (int64_t)sizeof path) {
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    memcpy(buf, path, sizeof path);
    return (int64_t)sizeof path - 1;
}

#endif /* !BURROW_OS_WINDOWS */

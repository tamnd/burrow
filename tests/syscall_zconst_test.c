/* syscall's generated constants, compared with the system's.
 *
 * Go's zerrors and zsysnum files are made by running the C preprocessor over
 * the system's headers, so every constant whose name is also a macro in those
 * headers has to come out the same as the macro. This includes the headers
 * Go's mkerrors.sh does, and tests/syscall_zconst_check.inc, which
 * tools/gen-syscall-tables.sh writes along with the constants, compares every
 * name both sides define. The two values are compared at the width of the
 * narrower of their two types, so a 32-bit value is the same whether one side
 * is signed and the other not, as Windows' HRESULT and HKEY constants are.
 *
 * The system's headers are not the ones Go read, so a constant can differ for
 * a reason that is not a mistake, such as a value a newer kernel changed.
 * Those are listed in known_differences with the reason, and the test logs
 * them.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if defined(__linux__)
#define _GNU_SOURCE
#endif

/* winsock2.h has to come before windows.h, which burrow's headers bring in. */
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "burrow/burrow.h"
#include "burrow/syscall.h"

#include <string.h>

#if defined(BURROW_OS_WINDOWS)
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(BURROW_OS_LINUX) && !defined(BURROW_OS_COSMO)
#include <dirent.h>
#include <fcntl.h>
/* The kernel's own headers are a package of their own on some systems, such as
 * linux-headers on Alpine, and the comparison does without them. */
#if defined(__has_include)
#if __has_include(<linux/netlink.h>)
#define HAVE_LINUX_HEADERS 1
#endif
#endif
#if defined(HAVE_LINUX_HEADERS)
#include <linux/filter.h>
#include <linux/icmpv6.h>
#include <linux/if.h>
#include <linux/if_addr.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/if_tun.h>
#include <linux/netlink.h>
#include <linux/reboot.h>
#include <linux/rtnetlink.h>
#include <linux/sched.h>
#include <linux/serial.h>
#include <linux/wait.h>
#endif
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#elif defined(BURROW_OS_DARWIN) || defined(BURROW_OS_FREEBSD) ||                       \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_OPENBSD)
#include <dirent.h>
#include <fcntl.h>
#include <net/bpf.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#endif
#if defined(BURROW_OS_FREEBSD)
/* sys/sockio.h spells SIOCGETSGCNT and SIOCGETVIFCNT with two structs that only
 * this header defines. */
#include <netinet/ip_mroute.h>
#endif

#include "check.h"

/* Names whose value here is Go's and differs from the system's on purpose,
 * with the reason. */
typedef struct KnownDifference {
    const char *name;
    const char *why;
} KnownDifference;

static const KnownDifference known_differences[] = {
#if defined(BURROW_OS_LINUX)
    /* Go's syscall is frozen, and its tables come from older kernel headers.
     * Newer kernels added families, attributes and message types, so these
     * counts went up. */
    {"AF_MAX", "newer kernel"},
    {"IFA_MAX", "newer kernel"},
    {"IFLA_MAX", "newer kernel"},
    {"RTAX_MAX", "newer kernel"},
    {"RTA_MAX", "newer kernel"},
    {"RTM_MAX", "newer kernel"},
    {"RTM_NR_FAMILIES", "newer kernel"},
    {"RTM_NR_MSGTYPES", "newer kernel"},
    {"MS_RMT_MASK", "newer kernel"},
    /* 4096 since Linux 5.4. */
    {"SOMAXCONN", "newer kernel"},
    /* Go's tables for some ports, such as s390x, are older than amd64's and
     * miss flags newer kernels added to these masks. */
    {"RTAX_FEATURE_MASK", "newer kernel"},
    {"RTNH_COMPARE_MASK", "newer kernel"},
    /* Go's arm64 table has amd64's value, which has amd64's O_DIRECTORY bit
     * in it. */
    {"O_TMPFILE", "amd64's value in Go's table"},
#if !defined(__GLIBC__)
    /* Go's come from glibc. musl counts O_PATH in O_ACCMODE, and its
     * O_LARGEFILE is the kernel's bit where glibc has 0 on 64-bit systems. */
    {"O_ACCMODE", "musl's own"},
    {"O_LARGEFILE", "musl's own"},
#endif
#elif defined(BURROW_OS_DARWIN)
    /* The same for macOS: Go's tables come from an older SDK, and these are
     * counts and limits that went up since, or a default Apple changed. */
    {"AF_MAX", "newer SDK"},
    {"EVFILT_SYSCOUNT", "newer SDK"},
    {"EVFILT_THREADMARKER", "newer SDK"},
    {"IPV6_FRAGTTL", "newer SDK"},
    {"NET_RT_MAXID", "newer SDK"},
    {"SYS_MAXSYSCALL", "newer SDK"},
    {"TCP_MAX_SACK", "newer SDK"},
#elif defined(BURROW_OS_WINDOWS)
    /* Go's are the numbers 0x80000000 and up. The SDK makes them pointers
     * from a 32-bit LONG, so on 64-bit Windows they are sign extended, and
     * Windows takes either. */
    {"HKEY_CLASSES_ROOT", "sign extended in the SDK"},
    {"HKEY_CURRENT_CONFIG", "sign extended in the SDK"},
    {"HKEY_CURRENT_USER", "sign extended in the SDK"},
    {"HKEY_DYN_DATA", "sign extended in the SDK"},
    {"HKEY_LOCAL_MACHINE", "sign extended in the SDK"},
    {"HKEY_PERFORMANCE_DATA", "sign extended in the SDK"},
    {"HKEY_USERS", "sign extended in the SDK"},
#endif
    {NULL, NULL},
};

static const char *known(const char *name) {
    for (size_t i = 0; known_differences[i].name != NULL; i++)
        if (strcmp(known_differences[i].name, name) == 0)
            return known_differences[i].why;
    return NULL;
}

static Int compared;

static unsigned long long low_bits(unsigned long long v, size_t size) {
    if (size >= sizeof v)
        return v;
    return v & ((1ull << (size * 8)) - 1);
}

static void check_const(TestingT *t, const char *name, unsigned long long got,
                        size_t got_size, unsigned long long want, size_t want_size) {
    compared++;
    size_t size = got_size < want_size ? got_size : want_size;
    if (low_bits(got, size) == low_bits(want, size))
        return;
    const char *why = known(name);
    if (why != NULL)
        testing_t_logf_v(t, "SYSCALL_%s = %#x, the system's is %#x (%s)",
                         str_from_cstr(name), got, want, str_from_cstr(why));
    else
        testing_t_errorf_v(t, "SYSCALL_%s = %#x, the system's %s is %#x",
                           str_from_cstr(name), got, str_from_cstr(name), want);
}

#define CHECK_CONST(n)                                                                 \
    check_const(t, #n, (unsigned long long)(SYSCALL_##n), sizeof(SYSCALL_##n),         \
                (unsigned long long)(n), sizeof(n))

static void TestConstantsMatchSystem(TestingT *t) {
#if defined(BURROW_OS_WASI)
    /* WASI has Linux's tables until syscall has wasip1's own, so there is
     * nothing to compare its headers with yet. */
    (void)check_const;
    testing_t_skip_v(t, "no wasip1 tables yet");
#endif
    compared = 0;
    /* macOS marks a few of the kqueue flags deprecated, and naming them is
     * the point here. Some of the system's macros mix signed and unsigned
     * themselves, such as Linux's NLA_HDRLEN. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "syscall_zconst_check.inc"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    testing_t_logf_v(t, "compared %d constants with the system's", compared);
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_DARWIN) || defined(BURROW_OS_WINDOWS)
    /* The release targets have hundreds in common with their headers, and
     * none at all would mean the headers above were not the right ones. */
    if (compared < 100)
        testing_t_errorf_v(t, "only %d constants compared", compared);
#endif
}

/* A few values written out, so a generator that went wrong everywhere at once
 * would still be caught. */
static void TestConstantsKnownValues(TestingT *t) {
#if defined(BURROW_OS_WINDOWS)
    CHECK(SYSCALL_AF_INET == 2);
    CHECK(SYSCALL_INVALID_FILE_ATTRIBUTES == 0xffffffff);
    CHECK(SYSCALL_GENERIC_READ == 0x80000000);
#elif defined(BURROW_OS_LINUX)
    CHECK(SYSCALL_AF_INET == 2);
    CHECK(SYSCALL_AF_INET6 == 10);
    CHECK(SYSCALL_O_CREAT == 0x40 || SYSCALL_O_CREAT == 0x100);
    CHECK(SYSCALL_SIZEOF_SOCKADDR_UNIX == 110);
    CHECK(SYSCALL_SIZEOF_SOCKADDR_INET4 == 16);
    CHECK(SYSCALL_SIZEOF_SOCKADDR_INET6 == 28);
#elif defined(BURROW_OS_DARWIN)
    CHECK(SYSCALL_AF_INET == 2);
    CHECK(SYSCALL_AF_INET6 == 30);
    CHECK(SYSCALL_O_CREAT == 0x200);
    CHECK(SYSCALL_SYS_EXIT == 1);
    CHECK(SYSCALL_SIZEOF_SOCKADDR_INET4 == 16);
#endif
    (void)t;
}

#define TESTS(X)                                                                       \
    X(TestConstantsMatchSystem)                                                        \
    X(TestConstantsKnownValues)

TESTING_MAIN(TESTS)

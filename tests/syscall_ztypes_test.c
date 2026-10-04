/* syscall's generated types, checked against Go's layout and the system's.
 *
 * tests/syscall_ztypes_check.inc, which tools/gen-syscall-tables.sh writes
 * along with the types, has the size of every type and the offset of every
 * field Go gives for each architecture, and the test checks the C compiler
 * agrees. The ones Go copied from the system's headers are compared with the
 * system's own structs as well.
 *
 * The system's headers come first, so a field name one of their macros would
 * spell differently stops the build here.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include <stddef.h>

#if defined(_WIN32)
/* winsock2.h brings in windows.h, and has to be first. */
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(__linux__) && !defined(__COSMOPOLITAN__)
#include <dirent.h>
#include <fcntl.h>
/* The kernel's own headers are a package of their own on some systems, such as
 * linux-headers on Alpine, and the test does without them. */
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
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||             \
    defined(__OpenBSD__)
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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#endif

#if !defined(_WIN32)
#include <sys/un.h>
#include <sys/utsname.h>
#endif

#include "burrow/burrow.h"
#include "burrow/syscall.h"

#include "check.h"

static Int checked;

static void check_size(TestingT *t, const char *type, size_t got, size_t want) {
    checked++;
    if (got != want)
        testing_t_errorf_v(t, "sizeof(%s) = %d, Go's is %d", str_from_cstr(type), got,
                           want);
}

static void check_offset(TestingT *t, const char *type, const char *field, size_t got,
                         size_t want) {
    checked++;
    if (got != want)
        testing_t_errorf_v(t, "%s.%s is at %d, Go's is at %d", str_from_cstr(type),
                           str_from_cstr(field), got, want);
}

#define CHECK_SIZE(T, n) check_size(t, #T, sizeof(T), n)
#define CHECK_OFFSET(T, f, n) check_offset(t, #T, #f, offsetof(T, f), n)

static void TestTypesLayout(TestingT *t) {
    checked = 0;
#include "syscall_ztypes_check.inc"
    testing_t_logf_v(t, "checked %d sizes and offsets", checked);
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_DARWIN) || defined(BURROW_OS_WINDOWS)
    if (checked < 100)
        testing_t_errorf_v(t, "only %d sizes and offsets checked", checked);
#endif
}

/* The system's struct is the one Go's was made from, so they have to agree. */
#define SAME_SIZE(ours, theirs)                                                        \
    do {                                                                               \
        if (sizeof(ours) != sizeof(theirs))                                            \
            testing_t_errorf_v(t, "sizeof(%s) = %d, sizeof(%s) = %d",                  \
                               str_from_cstr(#ours), sizeof(ours),                     \
                               str_from_cstr(#theirs), sizeof(theirs));                \
    } while (0)

#define SAME_OFFSET(ours, f, theirs, g)                                                \
    do {                                                                               \
        if (offsetof(ours, f) != offsetof(theirs, g))                                  \
            testing_t_errorf_v(t, "%s.%s is at %d, %s.%s at %d", str_from_cstr(#ours), \
                               str_from_cstr(#f), offsetof(ours, f),                   \
                               str_from_cstr(#theirs), str_from_cstr(#g),              \
                               offsetof(theirs, g));                                   \
    } while (0)

static void TestTypesMatchSystem(TestingT *t) {
#if defined(BURROW_OS_LINUX) && !defined(BURROW_OS_COSMO)
    SAME_SIZE(SyscallStat_t, struct stat);
    SAME_OFFSET(SyscallStat_t, size, struct stat, st_size);
    SAME_OFFSET(SyscallStat_t, mtim, struct stat, st_mtim);
    SAME_SIZE(SyscallTimespec, struct timespec);
    SAME_SIZE(SyscallTimeval, struct timeval);
    SAME_SIZE(SyscallRusage, struct rusage);
    SAME_SIZE(SyscallRawSockaddrInet4, struct sockaddr_in);
    SAME_SIZE(SyscallRawSockaddrInet6, struct sockaddr_in6);
    SAME_OFFSET(SyscallRawSockaddrInet6, scope_id, struct sockaddr_in6, sin6_scope_id);
    SAME_SIZE(SyscallRawSockaddrUnix, struct sockaddr_un);
    SAME_SIZE(SyscallLinger, struct linger);
    SAME_SIZE(SyscallIovec, struct iovec);
    SAME_SIZE(SyscallMsghdr, struct msghdr);
    SAME_OFFSET(SyscallMsghdr, controllen, struct msghdr, msg_controllen);
    SAME_SIZE(SyscallCmsghdr, struct cmsghdr);
    SAME_SIZE(SyscallUtsname, struct utsname);
    SAME_SIZE(SyscallTermios, struct termios);
    SAME_SIZE(SyscallEpollEvent, struct epoll_event);
    SAME_SIZE(SyscallRlimit, struct rlimit);
#elif defined(BURROW_OS_DARWIN)
    SAME_SIZE(SyscallStat_t, struct stat);
    SAME_OFFSET(SyscallStat_t, size, struct stat, st_size);
    SAME_OFFSET(SyscallStat_t, mtimespec, struct stat, st_mtimespec);
    SAME_SIZE(SyscallTimespec, struct timespec);
    SAME_SIZE(SyscallTimeval, struct timeval);
    SAME_SIZE(SyscallRusage, struct rusage);
    SAME_SIZE(SyscallRawSockaddrInet4, struct sockaddr_in);
    SAME_SIZE(SyscallRawSockaddrInet6, struct sockaddr_in6);
    SAME_SIZE(SyscallRawSockaddrUnix, struct sockaddr_un);
    SAME_SIZE(SyscallKevent_t, struct kevent);
    SAME_SIZE(SyscallMsghdr, struct msghdr);
    SAME_SIZE(SyscallTermios, struct termios);
    SAME_SIZE(SyscallRlimit, struct rlimit);
#elif defined(BURROW_OS_WINDOWS)
    SAME_SIZE(SyscallFiletime, FILETIME);
    SAME_SIZE(SyscallSystemtime, SYSTEMTIME);
    SAME_SIZE(SyscallByHandleFileInformation, BY_HANDLE_FILE_INFORMATION);
    SAME_SIZE(SyscallOverlapped, OVERLAPPED);
    SAME_SIZE(SyscallSecurityAttributes, SECURITY_ATTRIBUTES);
    SAME_SIZE(SyscallStartupInfo, STARTUPINFOW);
    SAME_OFFSET(SyscallStartupInfo, std_err, STARTUPINFOW, hStdError);
    SAME_SIZE(SyscallProcessInformation, PROCESS_INFORMATION);
    SAME_SIZE(SyscallWSAData, WSADATA);
    SAME_SIZE(SyscallRawSockaddrInet6, struct sockaddr_in6);
#endif
    (void)t;
}

#define TESTS(X)                                                                       \
    X(TestTypesLayout)                                                                 \
    X(TestTypesMatchSystem)

TESTING_MAIN(TESTS)

/* netFD and socket, the part of net between the poll FD and the connection
 * types. See internal.h.
 *
 * Derived from Go's src/net/fd_posix.go, fd_unix.go, sock_posix.go,
 * ipsock_posix.go, sockopt_linux.go, sockopt_bsd.go, sockopt_windows.go and
 * net.go's listenerBacklog.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/atomic.h"
#include "burrow/error.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/platform.h"
#include "burrow/syscall.h"

#include "../os/internal.h"

#include <stdint.h>
#include <string.h>

#define NF_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)

/* ------------------------------------------------------------------ errors */

/* wrapSyscallError: an Errno gets the name of the call it came from, and
 * anything else, which is one of the poll FD's own errors, is left alone. */
static Error nf_wrap(Str call, Error err) {
    if (err.vt != NULL && err.vt->self_type == TYPE_SYSCALL_ERRNO)
        return os_new_syscall_error(error_allocator(), call, err);
    return err;
}

/* os.NewSyscallError(call, errno). */
static Error nf_syscall_error(Str call, PalErrno pe) {
    return os_new_syscall_error(error_allocator(), call, burrow__os_errno(pe));
}

/* The call accept is in Go's errors, which is accept4 where Go makes the
 * socket non blocking in the same call and accept where it cannot. */
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_FREEBSD) ||                          \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_OPENBSD) ||                         \
    defined(BURROW_OS_DRAGONFLY)
#define NF_ACCEPT_CALL "accept4"
#else
#define NF_ACCEPT_CALL "accept"
#endif

/* readSyscallName, readFromSyscallName, writeSyscallName and
 * writeToSyscallName. */
#if defined(BURROW_OS_WINDOWS)
#define NF_READ_CALL "wsarecv"
#define NF_READ_FROM_CALL "wsarecvfrom"
#define NF_READ_MSG_CALL "wsarecvmsg"
#define NF_WRITE_CALL "wsasend"
#define NF_WRITE_TO_CALL "wsasendto"
#define NF_WRITE_MSG_CALL "wsasendmsg"
#define NF_WRITEV_CALL "wsasend"
#else
#define NF_READ_CALL "read"
#define NF_READ_FROM_CALL "recvfrom"
#define NF_READ_MSG_CALL "recvmsg"
#define NF_WRITE_CALL "write"
#define NF_WRITE_TO_CALL "sendto"
#define NF_WRITE_MSG_CALL "sendmsg"
#define NF_WRITEV_CALL "writev"
#endif

/* ---------------------------------------------------------------- the stack */

/* ipStackCapabilities, probed once. Two goroutines that probe at the same
 * time get the same answers and store the same word, so the probe needs no
 * lock, only a store that publishes the bits together with the flag that
 * says they are there. */
enum {
    NF_STACK_PROBED = 1U << 0,
    NF_STACK_IPV4 = 1U << 1,
    NF_STACK_IPV6 = 1U << 2,
    NF_STACK_IPV4MAP = 1U << 3
};

static uint32_t nf_stack;

#if !defined(BURROW_OS_WASI)
/* The probe's bind of ::1, or of 127.0.0.1 as an IPv4-mapped address, on an
 * IPv6 socket with IPV6_V6ONLY set to v6only. A bind that the system refuses
 * for want of permission still shows the address works. */
static bool nf_probe_bind(bool mapped) {
    PalErrno pe = PAL_OK;
    int64_t s = pal_socket(PAL_AF_INET6, PAL_SOCK_STREAM, PAL_IPPROTO_TCP, &pe);
    if (s < 0)
        return false;
    (void)pal_setsockopt(s, PAL_IPV6_V6ONLY, mapped ? 0 : 1, &pe);
    PalSockAddr sa = {0};
    sa.family = PAL_AF_INET6;
    if (mapped) {
        sa.addr[10] = 0xff;
        sa.addr[11] = 0xff;
        sa.addr[12] = 127;
        sa.addr[15] = 1;
    } else {
        sa.addr[15] = 1;
    }
    bool ok = pal_bind(s, &sa, &pe) || pe == PAL_EPERM || pe == PAL_EACCES;
    (void)pal_socket_close(s, &pe);
    return ok;
}
#endif

static uint32_t nf_stack_probe(void) {
    uint32_t bits = burrow__atomic_load_acquire_u32(&nf_stack);
    if (bits & NF_STACK_PROBED)
        return bits;
    bits = NF_STACK_PROBED;
#if defined(BURROW_OS_WASI)
    bits |= NF_STACK_IPV4 | NF_STACK_IPV6 | NF_STACK_IPV4MAP;
#else
    PalErrno pe = PAL_OK;
    int64_t s = pal_socket(PAL_AF_INET, PAL_SOCK_STREAM, PAL_IPPROTO_TCP, &pe);
    if (s >= 0) {
        (void)pal_socket_close(s, &pe);
        bits |= NF_STACK_IPV4;
    }
    if (nf_probe_bind(false))
        bits |= NF_STACK_IPV6;
    /* DragonFly and OpenBSD have no IPv4-mapped addresses on a socket, and
     * Go does not ask. */
#if !defined(BURROW_OS_DRAGONFLY) && !defined(BURROW_OS_OPENBSD)
    if (nf_probe_bind(true))
        bits |= NF_STACK_IPV4MAP;
#endif
#endif
    burrow__atomic_store_release_u32(&nf_stack, bits);
    return bits;
}

bool burrow__net_supports_ipv4(void) {
    return (nf_stack_probe() & NF_STACK_IPV4) != 0;
}

bool burrow__net_supports_ipv6(void) {
    return (nf_stack_probe() & NF_STACK_IPV6) != 0;
}

bool burrow__net_supports_ipv4map(void) {
    return (nf_stack_probe() & NF_STACK_IPV4MAP) != 0;
}

/* listenerBacklog, worked out by the first listener and kept. 0 until then,
 * and a racing second listener works it out again and gets the same. */
static uint32_t nf_backlog;

static int32_t nf_listener_backlog(void) {
    uint32_t n = burrow__atomic_load_relaxed_u32(&nf_backlog);
    if (n != 0)
        return (int32_t)n;
    int32_t max = pal_listen_backlog_max();
    if (max <= 0) {
#if defined(SYSCALL_SOMAXCONN)
        max = (int32_t)SYSCALL_SOMAXCONN;
#else
        max = 128;
#endif
    }
    burrow__atomic_store_relaxed_u32(&nf_backlog, (uint32_t)max);
    return max;
}

/* ------------------------------------------------------------- the options */

/* setDefaultSockopts: an IPv6 socket takes IPv4 as well unless ipv6only, and
 * a datagram socket may broadcast. Windows and the BSDs do the same, with
 * Windows leaving out the broadcast for a raw socket too. */
static Error nf_default_sockopts(int64_t s, int32_t family, int32_t sotype,
                                 bool ipv6only) {
    PalErrno pe = PAL_OK;
    if (family == PAL_AF_INET6 && sotype != PAL_SOCK_RAW)
        (void)pal_setsockopt(s, PAL_IPV6_V6ONLY, ipv6only ? 1 : 0, &pe);
    if ((sotype == PAL_SOCK_DGRAM || sotype == PAL_SOCK_RAW) && family != PAL_AF_UNIX) {
        if (!pal_setsockopt(s, PAL_SO_BROADCAST, 1, &pe))
            return nf_syscall_error(NF_LIT("setsockopt"), pe);
    }
    return BURROW_NO_ERROR;
}

/* setDefaultListenerSockopts: SO_REUSEADDR, so that a server can listen
 * again on the port it just closed. Windows reuses an address by default,
 * and there SO_REUSEADDR would let a second socket take a port a first one is
 * still using, so Go sets nothing. */
static Error nf_default_listener_sockopts(int64_t s) {
#if defined(BURROW_OS_WINDOWS)
    (void)s;
    return BURROW_NO_ERROR;
#else
    PalErrno pe = PAL_OK;
    if (!pal_setsockopt(s, PAL_SO_REUSEADDR, 1, &pe))
        return nf_syscall_error(NF_LIT("setsockopt"), pe);
    return BURROW_NO_ERROR;
#endif
}

/* setDefaultMulticastSockopts: SO_REUSEADDR, so that more than one socket
 * can listen to a group, and on the BSDs SO_REUSEPORT as well, which is what
 * lets them share the port there. */
static Error nf_default_multicast_sockopts(int64_t s) {
    PalErrno pe = PAL_OK;
    if (!pal_setsockopt(s, PAL_SO_REUSEADDR, 1, &pe))
        return nf_syscall_error(NF_LIT("setsockopt"), pe);
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) ||                             \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD) ||                         \
    defined(BURROW_OS_OPENBSD) || defined(BURROW_OS_DRAGONFLY) ||                      \
    defined(BURROW_OS_AIX)
    if (!pal_setsockopt(s, PAL_SO_REUSEPORT, 1, &pe))
        return nf_syscall_error(NF_LIT("setsockopt"), pe);
#endif
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------- the netFD */

/* newFD: a stream reads nothing at its end, and only a datagram or a raw
 * socket can read an empty message. */
static void nf_new(burrow__NetFD *fd, int64_t s, int32_t family, int32_t sotype,
                   Str net) {
    *fd = (burrow__NetFD){0};
    fd->pfd.sysfd = s;
    fd->family = family;
    fd->sotype = sotype;
    fd->net = net;
}

/* netFD.init. */
static Error nf_init(burrow__NetFD *fd) {
    int64_t s = fd->pfd.sysfd;
    bool stream = fd->sotype == PAL_SOCK_STREAM;
    bool eof = fd->sotype != PAL_SOCK_DGRAM && fd->sotype != PAL_SOCK_RAW;
    Error e = burrow__pfd_init(&fd->pfd, s, stream, eof);
    if (BURROW_FAILED(e)) {
        /* burrow__pfd_init cleared the FD, and the socket is still ours. */
        fd->pfd.sysfd = s;
        return e;
    }
    fd->polled = true;
    return BURROW_NO_ERROR;
}

Error burrow__netfd_close(burrow__NetFD *fd) {
    if (fd->polled)
        return burrow__pfd_close(&fd->pfd);
    if (fd->pfd.sysfd < 0)
        return net_err_closed;
    PalErrno pe = PAL_OK;
    bool ok = pal_socket_close(fd->pfd.sysfd, &pe);
    fd->pfd.sysfd = -1;
    return ok ? BURROW_NO_ERROR : burrow__os_errno(pe);
}

/* What the context.AfterFunc in connect does: makes the descriptor unwritable
 * so that the wait for the connect gives up at once. The connect waits for
 * this to finish before it lets go of fd, which Go leaves to the collector. */
typedef struct NfCancel {
    burrow__NetFD *fd;
    uint32_t done;
} NfCancel;

static void nf_cancel_connect(void *env) {
    NfCancel *c = (NfCancel *)env;
    (void)burrow__pfd_set_deadline(&c->fd->pfd, time_from_unix(1, 0),
                                   BURROW_POLL_WRITE);
    burrow__sema_release(&c->done, false);
}

/* A context's deadline, which is a reading of the monotonic clock, as a Time. */
static Time nf_mono_time(int64_t when) {
    return time_add(time_now(), when - burrow_nanotime());
}

/* Whether the channel is closed, without waiting. */
static bool nf_closed(Chan *c) {
    bool ok = true;
    return chan_try_recv(c, NULL, &ok) && !ok;
}

/* netFD.connect: a non blocking connect, and the wait for it, which ctx's
 * deadline and its cancellation cut short. What comes back in crsa is the
 * address the socket ended up connected to, when the system says, and family
 * PAL_AF_UNSPEC when it does not. */
static Error nf_connect(burrow__NetFD *fd, Context ctx, const PalSockAddr *rsa,
                        PalSockAddr *crsa) {
    crsa->family = PAL_AF_UNSPEC;
    PalErrno pe = PAL_OK;
    if (pal_connect(fd->pfd.sysfd, rsa, &pe) || pe == PAL_EISCONN) {
        /* Connected at once, which a connect to a local address may be. */
        return nf_init(fd);
    }
    if (pe != PAL_EINPROGRESS && pe != PAL_EALREADY && pe != PAL_EINTR)
        return nf_syscall_error(NF_LIT("connect"), pe);
    Error e = nf_init(fd);
    if (BURROW_FAILED(e))
        return e;

    Chan *done = ctx.vt != NULL ? context_done(ctx) : NULL;
    bool timed = false;
    NfCancel cancel = {fd, 0};
    StopFunc stop = {NULL, NULL};
    Context reg = {NULL, NULL};
    if (done != NULL) {
        int64_t when = 0;
        if (context_deadline(ctx, &when)) {
            e = burrow__pfd_set_deadline(&fd->pfd, nf_mono_time(when),
                                         BURROW_POLL_WRITE);
            if (BURROW_FAILED(e))
                return e;
            timed = true;
        }
        reg = context_after_func(heap_allocator(), ctx,
                                 BURROW_FN(Func, nf_cancel_connect, &cancel), &stop);
        if (reg.vt == NULL)
            e = burrow_err_out_of_memory;
    }
    while (BURROW_OK(e)) {
        /* Writable means the connect has finished, one way or the other, and
         * SO_ERROR says which. A connect that is still going, which can be
         * seen after a spurious wakeup, goes round again. */
        e = burrow__pfd_wait_write(&fd->pfd);
        if (BURROW_FAILED(e)) {
            if (done != NULL && nf_closed(done))
                e = burrow__net_map_err(context_err(ctx));
            break;
        }
        int64_t nerr = 0;
        if (!pal_getsockopt(fd->pfd.sysfd, PAL_SO_ERROR, &nerr, &pe)) {
            e = nf_syscall_error(NF_LIT("getsockopt"), pe);
            break;
        }
        PalErrno ce = (PalErrno)nerr;
        if (ce == PAL_EINPROGRESS || ce == PAL_EALREADY || ce == PAL_EINTR)
            continue;
        if (ce == PAL_EISCONN)
            break;
        if (ce == PAL_OK) {
            /* Writable with no error can still be a connect that has not
             * finished on some systems, and getpeername is what tells. */
            if (pal_getpeername(fd->pfd.sysfd, crsa, &pe))
                break;
            crsa->family = PAL_AF_UNSPEC;
            continue;
        }
        e = nf_syscall_error(NF_LIT("connect"), ce);
        break;
    }
    if (reg.vt != NULL) {
        if (!BURROW_CALLF0(stop)) {
            /* Too late to stop it: the descriptor has been made unwritable or
             * is about to be, so a connect that got through anyway is no
             * use, and fd has to outlive the function either way. */
            burrow__sema_acquire(&cancel.done, false, false);
            if (BURROW_OK(e))
                e = burrow__net_map_err(context_err(ctx));
        }
        context_release(reg);
    }
    if (timed)
        (void)burrow__pfd_set_deadline(&fd->pfd, (Time){0}, BURROW_POLL_WRITE);
    return e;
}

/* The address a socket is bound to, or family PAL_AF_UNSPEC when the system
 * will not say, which Go turns into a nil Addr. */
static void nf_sockname(burrow__NetFD *fd, PalSockAddr *out) {
    PalErrno pe = PAL_OK;
    if (!pal_getsockname(fd->pfd.sysfd, out, &pe))
        *out = (PalSockAddr){0};
}

BURROW_SENTINEL_ERROR(burrow__net_err_sockaddr_einval, "invalid argument");

/* to_sockaddr, with the EINVAL it stands in for turned into the error the
 * system call would have given. */
static Error nf_sockaddr(Str call, burrow__NetToSockaddr to_sockaddr, const void *addr,
                         int32_t family, PalSockAddr *out) {
    Error e = to_sockaddr(addr, family, out);
    if (e.vt == burrow__net_err_sockaddr_einval.vt &&
        e.data == burrow__net_err_sockaddr_einval.data)
        return nf_syscall_error(call, PAL_EINVAL);
    return e;
}

/* netFD.ctrlNetwork: the network a Control function is told, which names
 * the family for an IP network that did not. */
static Str nf_ctrl_network(const burrow__NetFD *fd, Alloc *a) {
    Str net = fd->net;
    if (fd->family == PAL_AF_UNIX || net.len == 0)
        return net;
    Byte last = net.p[net.len - 1];
    if (last == '4' || last == '6')
        return net;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)net.len + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, net.p, (size_t)net.len);
    p[net.len] = fd->family == PAL_AF_INET ? '4' : '6';
    return str_from_bytes(p, net.len + 1);
}

/* Hands fd to ctl's control function, if there is one, before the socket is
 * bound or connected, with the text of addr as the address. */
static Error nf_control(burrow__NetFD *fd, const burrow__NetSockCtl *ctl,
                        const void *addr, burrow__NetAddrText to_text) {
    if (ctl == NULL || ctl->ctrl.f == NULL)
        return BURROW_NO_ERROR;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str network = nf_ctrl_network(fd, a);
    Str address = addr != NULL ? to_text(addr, fd->family, a) : BURROW_STR_EMPTY;
    Error e = burrow_err_out_of_memory;
    if (network.len > 0 && (addr == NULL || address.len > 0)) {
        burrow__NetRawConn rc = {fd, {NULL, NULL}, {NULL, NULL}, false};
        Context ctx = ctl->ctx.vt != NULL ? ctl->ctx : context_background();
        e = BURROW_CALLF(ctl->ctrl, ctx, network, address, burrow__net_raw_conn(&rc));
    }
    arena_free(&ar);
    return e;
}

/* netFD.listenStream. */
static Error nf_listen_stream(burrow__NetFD *fd, const burrow__NetSockCtl *ctl,
                              const void *laddr, burrow__NetToSockaddr to_sockaddr,
                              burrow__NetAddrText to_text) {
    Error e = nf_default_listener_sockopts(fd->pfd.sysfd);
    if (BURROW_FAILED(e))
        return e;
    PalSockAddr lsa = {0};
    e = nf_sockaddr(NF_LIT("bind"), to_sockaddr, laddr, fd->family, &lsa);
    if (BURROW_FAILED(e))
        return e;
    e = nf_control(fd, ctl, laddr, to_text);
    if (BURROW_FAILED(e))
        return e;
    PalErrno pe = PAL_OK;
    if (!pal_bind(fd->pfd.sysfd, &lsa, &pe))
        return nf_syscall_error(NF_LIT("bind"), pe);
    if (!pal_listen(fd->pfd.sysfd, nf_listener_backlog(), &pe))
        return nf_syscall_error(NF_LIT("listen"), pe);
    e = nf_init(fd);
    if (BURROW_FAILED(e))
        return e;
    nf_sockname(fd, &fd->laddr);
    return BURROW_NO_ERROR;
}

/* netFD.listenDatagram. For a multicast group the caller has to_sockaddr
 * and to_text give the unspecified address with the group's port, as Go
 * binds there. */
static Error nf_listen_datagram(burrow__NetFD *fd, const burrow__NetSockCtl *ctl,
                                const void *laddr, bool group,
                                burrow__NetToSockaddr to_sockaddr,
                                burrow__NetAddrText to_text) {
    Error e = BURROW_NO_ERROR;
    if (group) {
        e = nf_default_multicast_sockopts(fd->pfd.sysfd);
        if (BURROW_FAILED(e))
            return e;
    }
    PalSockAddr lsa = {0};
    e = nf_sockaddr(NF_LIT("bind"), to_sockaddr, laddr, fd->family, &lsa);
    if (BURROW_FAILED(e))
        return e;
    e = nf_control(fd, ctl, laddr, to_text);
    if (BURROW_FAILED(e))
        return e;
    PalErrno pe = PAL_OK;
    if (!pal_bind(fd->pfd.sysfd, &lsa, &pe))
        return nf_syscall_error(NF_LIT("bind"), pe);
    e = nf_init(fd);
    if (BURROW_FAILED(e))
        return e;
    nf_sockname(fd, &fd->laddr);
    return BURROW_NO_ERROR;
}

/* netFD.dial. */
static Error nf_dial(burrow__NetFD *fd, const burrow__NetSockCtl *ctl,
                     const void *laddr, const void *raddr,
                     burrow__NetToSockaddr to_sockaddr, burrow__NetAddrText to_text) {
    Error e = nf_control(fd, ctl, raddr != NULL ? raddr : laddr, to_text);
    if (BURROW_FAILED(e))
        return e;
    PalErrno pe = PAL_OK;
    if (laddr != NULL) {
        PalSockAddr lsa = {0};
        e = nf_sockaddr(NF_LIT("bind"), to_sockaddr, laddr, fd->family, &lsa);
        if (BURROW_FAILED(e))
            return e;
        if (!pal_bind(fd->pfd.sysfd, &lsa, &pe))
            return nf_syscall_error(NF_LIT("bind"), pe);
    }
    PalSockAddr rsa = {0};
    PalSockAddr crsa = {0};
    if (raddr != NULL) {
        e = nf_sockaddr(NF_LIT("connect"), to_sockaddr, raddr, fd->family, &rsa);
        if (BURROW_FAILED(e))
            return e;
        Context ctx = ctl != NULL ? ctl->ctx : (Context){NULL, NULL};
        e = nf_connect(fd, ctx, &rsa, &crsa);
        if (BURROW_FAILED(e))
            return e;
        fd->is_connected = true;
    } else {
        e = nf_init(fd);
        if (BURROW_FAILED(e))
            return e;
    }
    /* The remote address is the one the connect ended at if the system said,
     * then whatever getpeername says, then the one asked for. */
    nf_sockname(fd, &fd->laddr);
    if (crsa.family != PAL_AF_UNSPEC)
        fd->raddr = crsa;
    else if (!pal_getpeername(fd->pfd.sysfd, &fd->raddr, &pe))
        fd->raddr = rsa;
    return BURROW_NO_ERROR;
}

Error burrow__netfd_socket(burrow__NetFD *fd, const burrow__NetSockCtl *ctl, Str net,
                           int32_t family, int32_t sotype, int32_t proto, bool ipv6only,
                           const void *laddr, const void *raddr, bool group,
                           burrow__NetToSockaddr to_sockaddr,
                           burrow__NetAddrText to_text) {
    *fd = (burrow__NetFD){0};
    fd->pfd.sysfd = -1;
    PalErrno pe = PAL_OK;
    int64_t s = pal_socket(family, sotype, proto, &pe);
    if (s < 0)
        return nf_syscall_error(NF_LIT("socket"), pe);
    Error e = nf_default_sockopts(s, family, sotype, ipv6only);
    if (BURROW_FAILED(e)) {
        (void)pal_socket_close(s, &pe);
        return e;
    }
    nf_new(fd, s, family, sotype, net);

    if (laddr != NULL && raddr == NULL &&
        (sotype == PAL_SOCK_STREAM || sotype == PAL_SOCK_SEQPACKET))
        e = nf_listen_stream(fd, ctl, laddr, to_sockaddr, to_text);
    else if (laddr != NULL && raddr == NULL && sotype == PAL_SOCK_DGRAM)
        e = nf_listen_datagram(fd, ctl, laddr, group, to_sockaddr, to_text);
    else
        e = nf_dial(fd, ctl, laddr, raddr, to_sockaddr, to_text);
    if (BURROW_FAILED(e)) {
        (void)burrow__netfd_close(fd);
        return e;
    }
    return BURROW_NO_ERROR;
}

Error burrow__netfd_accept(burrow__NetFD *fd, burrow__NetFD *out) {
    PalSockAddr rsa = {0};
    Error e = BURROW_NO_ERROR;
    int64_t s = burrow__pfd_accept(&fd->pfd, &rsa, &e);
    if (s < 0)
        return nf_wrap(NF_LIT(NF_ACCEPT_CALL), e);
    nf_new(out, s, fd->family, fd->sotype, fd->net);
    e = nf_init(out);
    if (BURROW_FAILED(e)) {
        (void)burrow__netfd_close(out);
        return e;
    }
    nf_sockname(out, &out->laddr);
    out->raddr = rsa;
    return BURROW_NO_ERROR;
}

/* --------------------------------------------------------------- the calls */

Int burrow__netfd_read(burrow__NetFD *fd, Slice p, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = burrow__pfd_read(&fd->pfd, p, &e);
    BURROW_OUT(err, nf_wrap(NF_LIT(NF_READ_CALL), e));
    return n;
}

Int burrow__netfd_write(burrow__NetFD *fd, Slice p, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = burrow__pfd_write(&fd->pfd, p, &e);
    BURROW_OUT(err, nf_wrap(NF_LIT(NF_WRITE_CALL), e));
    return n;
}

int64_t burrow__netfd_write_buffers(burrow__NetFD *fd, NetBuffers *v, Error *err) {
    Error e = BURROW_NO_ERROR;
    int64_t n = burrow__pfd_writev(&fd->pfd, v, &e);
    BURROW_OUT(err, nf_wrap(NF_LIT(NF_WRITEV_CALL), e));
    return n;
}

Int burrow__netfd_read_from(burrow__NetFD *fd, Slice p, PalSockAddr *from, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = burrow__pfd_read_from(&fd->pfd, p, from, &e);
    BURROW_OUT(err, nf_wrap(NF_LIT(NF_READ_FROM_CALL), e));
    return n;
}

Int burrow__netfd_write_to(burrow__NetFD *fd, Slice p, const PalSockAddr *to,
                           Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = burrow__pfd_write_to(&fd->pfd, p, to, &e);
    BURROW_OUT(err, nf_wrap(NF_LIT(NF_WRITE_TO_CALL), e));
    return n;
}

Int burrow__netfd_read_msg(burrow__NetFD *fd, Slice p, Slice oob, Int *oobn, Int *flags,
                           PalSockAddr *from, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = burrow__pfd_read_msg(&fd->pfd, p, oob, oobn, flags, from, &e);
    BURROW_OUT(err, nf_wrap(NF_LIT(NF_READ_MSG_CALL), e));
    return n;
}

Int burrow__netfd_write_msg(burrow__NetFD *fd, Slice p, Slice oob,
                            const PalSockAddr *to, Int *oobn, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = burrow__pfd_write_msg(&fd->pfd, p, oob, to, oobn, &e);
    BURROW_OUT(err, nf_wrap(NF_LIT(NF_WRITE_MSG_CALL), e));
    return n;
}

Error burrow__netfd_write_msg_error(Error err) {
    if (err.vt == burrow__net_err_sockaddr_einval.vt &&
        err.data == burrow__net_err_sockaddr_einval.data)
        return nf_syscall_error(NF_LIT(NF_WRITE_MSG_CALL), PAL_EINVAL);
    return err;
}

Error burrow__netfd_write_to_error(Error err) {
    if (err.vt == burrow__net_err_sockaddr_einval.vt &&
        err.data == burrow__net_err_sockaddr_einval.data)
        return nf_syscall_error(NF_LIT(NF_WRITE_TO_CALL), PAL_EINVAL);
    return err;
}

Error burrow__netfd_shutdown(burrow__NetFD *fd, int32_t how) {
    return nf_wrap(NF_LIT("shutdown"), burrow__pfd_shutdown(&fd->pfd, how));
}

Error burrow__netfd_setsockopt(burrow__NetFD *fd, int32_t opt, int64_t value) {
    Error e = burrow__pfd_incref(&fd->pfd);
    if (BURROW_FAILED(e))
        return e;
    PalErrno pe = PAL_OK;
    if (!pal_setsockopt(fd->pfd.sysfd, opt, value, &pe))
        e = burrow__os_errno(pe);
    Error d = burrow__pfd_decref(&fd->pfd);
    if (BURROW_OK(e))
        e = d;
    return nf_wrap(NF_LIT("setsockopt"), e);
}

Error burrow__netfd_setsockopt_mreq(burrow__NetFD *fd, int32_t opt, const PalMreq *m) {
    Error e = burrow__pfd_incref(&fd->pfd);
    if (BURROW_FAILED(e))
        return e;
    PalErrno pe = PAL_OK;
    if (!pal_setsockopt_mreq(fd->pfd.sysfd, opt, m, &pe))
        e = burrow__os_errno(pe);
    Error d = burrow__pfd_decref(&fd->pfd);
    if (BURROW_OK(e))
        e = d;
    return nf_wrap(NF_LIT("setsockopt"), e);
}

/* ---------------------------------------------------------------- rawConn */

static Error nf_raw_control(void *self, SyscallFdFunc f) {
    burrow__NetRawConn *c = (burrow__NetRawConn *)self;
    if (c == NULL || c->fd == NULL)
        return burrow__net_einval();
    Error e = burrow__pfd_raw_control(&c->fd->pfd, f);
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(NF_LIT("raw-control"), c->fd->net,
                                 (NetAddr){NULL, NULL}, c->laddr, e);
    return e;
}

static Error nf_raw_io(burrow__NetRawConn *c, SyscallFdDoneFunc f, bool read) {
    if (c == NULL || c->fd == NULL || c->listener)
        return burrow__net_einval();
    Error e = read ? burrow__pfd_raw_read(&c->fd->pfd, f)
                   : burrow__pfd_raw_write(&c->fd->pfd, f);
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(read ? NF_LIT("raw-read") : NF_LIT("raw-write"),
                                 c->fd->net, c->laddr, c->raddr, e);
    return e;
}

static Error nf_raw_read(void *self, SyscallFdDoneFunc f) {
    return nf_raw_io((burrow__NetRawConn *)self, f, true);
}

static Error nf_raw_write(void *self, SyscallFdDoneFunc f) {
    return nf_raw_io((burrow__NetRawConn *)self, f, false);
}

static const Type nf_raw_conn_desc = {
    {(const Byte *)"rawConn", 7},
    {(const Byte *)"net", 3},
    KIND_STRUCT,
    (uint32_t)sizeof(burrow__NetRawConn),
    (uint16_t)_Alignof(burrow__NetRawConn),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e727763U, /* "nrwc" */
    NULL,
};

static const SyscallRawConnVT nf_raw_conn_vt = {
    &nf_raw_conn_desc,
    nf_raw_control,
    nf_raw_read,
    nf_raw_write,
};

SyscallRawConn burrow__net_raw_conn(burrow__NetRawConn *rc) {
    return (SyscallRawConn){&nf_raw_conn_vt, rc};
}

/* What the files of net share with each other and with nothing else.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_NET_INTERNAL_H
#define BURROW_SRC_NET_INTERNAL_H

#include "burrow/error.h"
#include "burrow/fdmutex.h"
#include "burrow/netpoll.h"
#include "burrow/own.h"
#include "burrow/pal.h"
#include "burrow/slice.h"
#include "burrow/time.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ poll.FD
 *
 * Go's internal/poll.FD for a socket: the descriptor, the fdMutex that keeps
 * it alive while a call is using it, and its place in the netpoller. Every
 * call below takes the lock it needs, makes the system call, and when the
 * answer is that it would block, parks the goroutine in the poller until the
 * descriptor is ready and tries again. So a goroutine blocked on a connection
 * holds a stack and no thread, and a Close from another goroutine wakes it
 * with net_err_closed.
 *
 * The errors are Go's. A system call that fails gives its Errno, which net
 * wraps in an os.SyscallError and then a net.OpError. A call on a closed FD
 * gives net_err_closed, and one that ran past its deadline gives
 * os_err_deadline_exceeded.
 *
 * Only the readiness half is here, which covers everything but Windows. On
 * Windows a socket is read by handing the kernel the read and waiting for it
 * to finish, which is a different file in Go and will be one here, and until
 * then burrow__pfd_init answers ENOSYS there.
 *
 * Every call that can wait has to be made from a goroutine. */
typedef struct burrow__PollFD {
    burrow__FdMutex mu;
    int64_t sysfd;
    burrow__PollDesc *pd;
    uint32_t csema;

    /* Go's IsStream, which caps one system call at a gigabyte, and
     * ZeroReadIsEOF, which makes a read of nothing io_eof. A TCP or Unix
     * stream socket has both. A datagram socket has neither, since an empty
     * datagram is a datagram. */
    bool stream;
    bool zero_read_is_eof;
} burrow__PollFD;

/* poll.ErrNotPollable, for a descriptor the poller turned down. */
extern const Error burrow__net_err_not_pollable;

/* poll.FD.Init: takes sysfd, which has to be non blocking already, into the
 * poller. On an error the FD is not usable and sysfd is still the caller's to
 * close. */
Error burrow__pfd_init(burrow__PollFD *fd, int64_t sysfd, bool stream,
                       bool zero_read_is_eof);

/* poll.FD.Close: marks it closed, wakes every goroutine blocked in it, and
 * waits until the last of them has let go before closing the descriptor.
 * net_err_closed when it was closed already, and the close's own error
 * otherwise. */
Error burrow__pfd_close(burrow__PollFD *fd);

/* The reference a call that neither reads nor writes takes, and gives back.
 * incref gives net_err_closed after a close, and decref gives the close error
 * when it was the last reference after one. */
Error burrow__pfd_incref(burrow__PollFD *fd);
Error burrow__pfd_decref(burrow__PollFD *fd);

/* poll.FD.Read and ReadFrom. A read of an empty p does nothing, and a read
 * of nothing at all on a stream is io_eof. from may be NULL. */
Int burrow__pfd_read(burrow__PollFD *fd, Slice p, Error *err);
Int burrow__pfd_read_from(burrow__PollFD *fd, Slice p, PalSockAddr *from, Error *err);

/* poll.FD.Write, which writes all of p unless something fails, and WriteTo,
 * which sends one datagram to `to`. */
Int burrow__pfd_write(burrow__PollFD *fd, Slice p, Error *err);
Int burrow__pfd_write_to(burrow__PollFD *fd, Slice p, const PalSockAddr *to,
                         Error *err);

/* poll.FD.Accept: the next connection, non blocking and not inherited, with
 * the address of the other end in peer, which may be NULL. -1 on an error. A
 * connection that was aborted before it could be accepted is skipped, as Go
 * does. */
int64_t burrow__pfd_accept(burrow__PollFD *fd, PalSockAddr *peer, Error *err);

/* poll.FD.Shutdown, with PAL_SHUT_RD, PAL_SHUT_WR or PAL_SHUT_RDWR. */
Error burrow__pfd_shutdown(burrow__PollFD *fd, int32_t how);

/* SetDeadline, SetReadDeadline and SetWriteDeadline in one, with mode
 * BURROW_POLL_READ, BURROW_POLL_WRITE or both. The zero Time clears it. */
Error burrow__pfd_set_deadline(burrow__PollFD *fd, Time t, uint32_t mode);

/* poll.FD.WaitWrite: waits until the descriptor is writable, which is how a
 * non blocking connect is waited for. Takes no lock, as in Go, because the
 * caller is the only one who has the FD yet. */
Error burrow__pfd_wait_write(burrow__PollFD *fd);

/* ------------------------------------------------------------------- netFD
 *
 * Go's netFD: a poll FD with what net knows about the socket around it, the
 * family and type it was made with, the network name the caller asked for,
 * and the two addresses. TCPConn, TCPListener and the rest are a netFD and
 * the methods that turn its answers into Go's errors.
 *
 * The errors that come out of here are what Go's netFD gives: an Errno from
 * a system call is wrapped in an os.SyscallError naming the call, and
 * net_err_closed, os_err_deadline_exceeded and io_eof come through as they
 * are. The OpError around all of it is the caller's to add, because the
 * caller knows the operation's name. */
typedef struct burrow__NetFD {
    burrow__PollFD pfd;

    /* Whether pfd went through burrow__pfd_init. Go makes the socket first
     * and hands it to the poller only once it is bound and listening or
     * connected, and a socket that fails before then is closed by hand. */
    bool polled;
    bool is_connected;
    int32_t family;
    int32_t sotype;

    /* The network the caller asked for, such as "tcp4". A literal, since the
     * caller's Str may not live as long as the socket does. */
    Str net;

    /* The addresses, family PAL_AF_UNSPEC when there is none. */
    PalSockAddr laddr;
    PalSockAddr raddr;
} burrow__NetFD;

/* Go's sockaddr interface, cut down to the one method socket needs: the
 * address addr for a socket of family, in out. Fails with a NetAddrError for
 * an address that family cannot hold, such as an IPv6 address on an IPv4
 * socket. addr is never NULL when this is called. */
typedef Error (*burrow__NetToSockaddr)(const void *addr, int32_t family,
                                       PalSockAddr *out);

/* What a burrow__NetToSockaddr gives for an address the system call would
 * turn down with EINVAL, such as a port past 65535, so that the caller can
 * report it as that call's error, the way Go's syscall package does. */
extern const Error burrow__net_err_sockaddr_einval;

/* Go's socket: makes a socket and then, with laddr and no raddr on a stream,
 * binds and listens on it, and otherwise binds to laddr if there is one and
 * connects to raddr. laddr and raddr are whatever to_sockaddr takes, and
 * either may be NULL. A connect waits, from a goroutine, until it is done or
 * deadline passes, and the zero Time is no deadline. On an error nothing is
 * left open. */
Error burrow__netfd_socket(burrow__NetFD *fd, Str net, int32_t family, int32_t sotype,
                           int32_t proto, bool ipv6only, const void *laddr,
                           const void *raddr, burrow__NetToSockaddr to_sockaddr,
                           Time deadline);

/* netFD.accept: the next connection, in out, with both its addresses. */
Error burrow__netfd_accept(burrow__NetFD *fd, burrow__NetFD *out);

/* netFD.Close. A socket that never reached the poller is closed at once. */
Error burrow__netfd_close(burrow__NetFD *fd);

/* netFD.Read and Write. */
Int burrow__netfd_read(burrow__NetFD *fd, Slice p, Error *err);
Int burrow__netfd_write(burrow__NetFD *fd, Slice p, Error *err);

/* netFD.shutdown, with PAL_SHUT_RD or PAL_SHUT_WR. */
Error burrow__netfd_shutdown(burrow__NetFD *fd, int32_t how);

/* poll.FD.SetsockoptInt with the error wrapped the way net's setters wrap
 * it, as "setsockopt". */
Error burrow__netfd_setsockopt(burrow__NetFD *fd, int32_t opt, int64_t value);

/* Whether this machine can make IPv4 sockets, and IPv6 sockets that take
 * IPv4 as well, which decides the family of a listener on every address.
 * Probed once, as Go does. */
bool burrow__net_supports_ipv4(void);
bool burrow__net_supports_ipv4map(void);

#endif /* BURROW_SRC_NET_INTERNAL_H */

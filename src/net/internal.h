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

#endif /* BURROW_SRC_NET_INTERNAL_H */

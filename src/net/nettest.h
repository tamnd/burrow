/* internal/nettest, a connection and a listener that live in memory, for
 * tests. net/http/httptest's NewTestServer serves on them.
 *
 * Go's package also has a PacketConn, which nothing here uses yet and which is
 * not ported.
 *
 * Go leaves the memory to its collector. Here a pair of Conns is given back
 * when both ends have been through burrow__nettest_conn_free, and a Listener
 * with burrow__nettest_listener_free, which frees the server ends nobody has
 * accepted. Addresses are borrowed: one given to set_local_addr or set_addr
 * has to outlive every Conn that has it, and a Listener has to outlive the
 * Conns it made unless set_addr gave it an address that does.
 *
 * Derived from Go's src/internal/nettest. Go source: go1.27.3.
 *
 * Copyright 2026 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_NET_NETTEST_H
#define BURROW_SRC_NET_NETTEST_H

/* burrow:package internal/nettest */

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/time.h"

#include <stdbool.h>

/* Conn is an in-memory test implementation of net.Conn.
 *
 * Conns come in pairs. Writes to one Conn are read by its peer, and
 * vice-versa. */
typedef struct NettestConn NettestConn;

/* Listener is an in-memory test implementation of net.Listener. */
typedef struct NettestListener NettestListener;

BURROW_FUNC(NettestConnFunc, void, NettestConn *conn);

/* errClosedByPeer, what a Write gives once the peer has closed its reading
 * side, wrapped in an OpError. */
extern const Error burrow__nettest_err_closed_by_peer;

/* NewConnPair returns a pair of connected Conns.
 *
 * Both are NULL when a cannot spare the memory. */
void burrow__nettest_new_conn_pair(Alloc *a, NettestConn **c1, NettestConn **c2);

/* Gives back c's end of the pair, once nothing is using it. The pair's memory
 * goes back when both ends have been given back. Not in Go. */
void burrow__nettest_conn_free(NettestConn *c);

/* c as a net.Conn, borrowing it. */
NetConn burrow__nettest_conn_as_net_conn(NettestConn *c);

/* Go's conn.(*nettest.Conn): the Conn behind c, or NULL if c is not one. */
NettestConn *burrow__nettest_conn_from_net_conn(NetConn c);

/* Peer returns the other end of the connection. */
NettestConn *burrow__nettest_conn_peer(NettestConn *c);

/* Read reads data from the connection. */
Int burrow__nettest_conn_read(NettestConn *c, Slice b, Error *err);

/* CanRead reports whether Read can proceed without blocking. */
bool burrow__nettest_conn_can_read(NettestConn *c);

/* Write writes data to the connection. */
Int burrow__nettest_conn_write(NettestConn *c, Slice b, Error *err);

/* IsClosed reports whether the connection has been closed.
 * A connection is closed if CloseRead and CloseWrite are both called,
 * or if Close is called.
 *
 * To identify when the other side of the Conn has been closed,
 * use Conn.Peer().IsClosed(). */
bool burrow__nettest_conn_is_closed(NettestConn *c);

/* CloseRead shuts down the reading side of the connection. */
Error burrow__nettest_conn_close_read(NettestConn *c);

/* CloseWrite shuts down the writing side of the connection. */
Error burrow__nettest_conn_close_write(NettestConn *c);

/* Close closes the connection. */
Error burrow__nettest_conn_close(NettestConn *c);

/* SetCloseError sets the error returned by Close.
 * Close still closes the connection.
 * A nil error restores the usual behavior. */
void burrow__nettest_conn_set_close_error(NettestConn *c, Error err);

/* LocalAddr returns the (fake) local network address. */
NetAddr burrow__nettest_conn_local_addr(NettestConn *c);

/* SetLocalAddr sets the local address.
 *
 * To set the remote address, set the local address of Conn's peer. */
void burrow__nettest_conn_set_local_addr(NettestConn *c, NetAddr addr);

/* LocalAddr returns the (fake) remote network address. */
NetAddr burrow__nettest_conn_remote_addr(NettestConn *c);

/* SetDeadline sets the read and write deadlines for the connection. */
Error burrow__nettest_conn_set_deadline(NettestConn *c, Time t);

/* SetReadDeadline sets the read deadline for the connection. */
Error burrow__nettest_conn_set_read_deadline(NettestConn *c, Time t);

/* SetWriteDeadline sets the write deadline for the connection. */
Error burrow__nettest_conn_set_write_deadline(NettestConn *c, Time t);

/* SetReadBufferSize sets the connection's read buffer.
 * Writes to the other end of the connection will block so long as the buffer is full.
 * Setting the size to 0 blocks all writes until the size is increased. */
void burrow__nettest_conn_set_read_buffer_size(NettestConn *c, Int size);

/* SetReadError causes any currently blocked and future Read calls to return
 * a net.OpError wrapping err. It does not affect the other half of the connection.
 * Reads will return any buffered data before returning the error,
 * including data written after the error is set and io.EOF after the other end is closed.
 * A nil error restores the usual behavior. */
void burrow__nettest_conn_set_read_error(NettestConn *c, Error err);

/* SetWriteError causes any currently blocked and future Write calls to return
 * a net.OpError wrapping err. It does not affect the other half of the connection.
 * Writes will not write data to the connection buffer while an error is set.
 * A nil error restores the usual behavior. */
void burrow__nettest_conn_set_write_error(NettestConn *c, Error err);

/* NewListener returns a new Listener.
 *
 * NULL when a cannot spare the memory. The Conns it makes come from a too. */
BURROW_OWNS(ret) NettestListener *burrow__nettest_new_listener(Alloc *a);

/* Gives back li and the server ends of its Conns that nobody has accepted.
 * Not in Go. */
void burrow__nettest_listener_free(NettestListener *li);

/* li as a net.Listener, borrowing it. */
NetListener burrow__nettest_listener_as_net_listener(NettestListener *li);

/* Close closes the listener.
 * Any blocked Accept operations will be unblocked and return errors. */
Error burrow__nettest_listener_close(NettestListener *li);

/* Addr returns the listener's network address.
 *
 * The address is always a *net.TCPAddr. */
NetAddr burrow__nettest_listener_addr(NettestListener *li);

/* SetAddr sets the listener's network address. */
void burrow__nettest_listener_set_addr(NettestListener *li, NetAddr addr);

/* NewConn returns a new connection to the listener.
 *
 * Accept will return the other side of the conn.
 *
 * NULL when the memory for it cannot be had. */
NettestConn *burrow__nettest_listener_new_conn(NettestListener *li);

/* NewConnConfig returns a new connection to the listener.
 *
 * The function f is called with the new client connection.
 * After f returns, Accept will return the other side of the connection.
 *
 * NULL when the memory for it cannot be had, and then f is not called. */
NettestConn *burrow__nettest_listener_new_conn_config(NettestListener *li,
                                                      NettestConnFunc f);

/* Accept waits for and returns the next connection to the listener. The
 * caller owns the Conn it gives, and frees it with burrow__nettest_conn_free.
 *
 * The net.Listener's accept gives the same Conn as a NetConn. */
NettestConn *burrow__nettest_listener_accept(NettestListener *li, Error *err);

/* SetAcceptError causes any currently blocked and future Accept calls to return
 * a net.OpError wrapping err.
 * Accept will return any available connections before returning the error,
 * including connections created after the error is set.
 * A nil error restores the usual behavior. */
void burrow__nettest_listener_set_accept_error(NettestListener *li, Error err);

/* SetCloseError sets the error returned by Close.
 * Close still closes the listener.
 * A nil error restores the usual behavior. */
void burrow__nettest_listener_set_close_error(NettestListener *li, Error err);

#endif /* BURROW_SRC_NET_NETTEST_H */

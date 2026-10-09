# Networking

`burrow/net/netip.h` is Go's `net/netip`, which holds IP addresses, address and port pairs, and CIDR prefixes as small values. `burrow/net/url.h` is Go's `net/url`, which parses, builds and resolves URLs and query strings. `burrow/net.h` has the address types from Go's `net` package itself, `IP`, `IPMask` and `IPNet`, with `SplitHostPort` and `JoinHostPort`. The sockets, `Dial` and `Listen` sit on top of the runtime's network poller, and `net/http` and the rest will land in this guide as they are ported.

## Addresses, ports and prefixes

There are three types. `NetipAddr` is an IPv4 or IPv6 address, with an IPv6 zone when there is one. `NetipAddrPort` is an address and a port, and `NetipPrefix` is an address and a prefix length, which is what `10.0.0.0/8` writes down. All three are a few machine words, are passed and returned by value, and never allocate:

<!-- example: ../examples/net/netip.c#parse -->
```c
Error err;
NetipAddr ip = netip_parse_addr(BURROW_S("192.168.1.20"), &err);
NetipPrefix lan = netip_parse_prefix(BURROW_S("192.168.0.0/16"), &err);
NetipAddrPort ap = netip_parse_addr_port(BURROW_S("[fe80::1%eth0]:8080"), &err);
printf("%d %d\n", netip_prefix_contains(lan, ip),
       netip_addr_is_private(ip)); /* 1 1 */
fmt_printf_v("%v %v %v\n", BURROW_ANY(TYPE_NETIP_ADDR, &ip),
             BURROW_ANY(TYPE_NETIP_PREFIX, &lan),
             BURROW_ANY(TYPE_NETIP_ADDR_PORT, &ap));
netip_parse_addr(BURROW_S("300.1.2.3"), &err);
fmt_printf_v("%v\n", err);
```

That prints:

```
1 1
192.168.1.20 192.168.0.0/16 [fe80::1%eth0]:8080
ParseAddr("300.1.2.3"): IPv4 field has value >255
```

The parse functions take an `Error *` like everything else in the library, and the error text is Go's, word for word. `netip_must_parse_addr`, `netip_must_parse_addr_port` and `netip_must_parse_prefix` panic instead, which is what you want for constants in code and tests.

C has no `==` on structs, so compare with `netip_addr_eq`, `netip_addr_port_eq` and `netip_prefix_eq`, or with the `_compare` functions when you need an order. Order is Go's: the zero value first, then IPv4, then IPv6, and by zone after the bits. The zero `NetipAddr` is not a valid address, and is neither `0.0.0.0` nor `::`. `netip_addr_is_valid` tells them apart.

Each type has a descriptor (`TYPE_NETIP_ADDR`, `TYPE_NETIP_ADDR_PORT` and `TYPE_NETIP_PREFIX`), so `fmt` prints them with `%v` and `%s`, and they work as `Map` keys. Two equal addresses are equal byte for byte, because the zone is kept as a `unique` handle.

## Text

`netip_addr_string` and the other string functions allocate from the allocator you pass. The `append_to` functions write into a slice you already have, and never allocate when it has room, which is the fast way to build a line of output:

<!-- example: ../examples/net/netip.c#text -->
```c
NetipAddr ip = netip_must_parse_addr(BURROW_S("2001:db8::1"));
Str s = netip_addr_string(ip, a);
Str long_form = netip_addr_string_expanded(ip, a);
printf("%.*s\n%.*s\n", (int)s.len, s.p, (int)long_form.len, long_form.p);

Byte buf[64];
Slice b = slice_from(buf, 0, sizeof buf, TYPE_BYTE);
NetipPrefix p = netip_must_parse_prefix(BURROW_S("10.1.2.3/8"));
b = netip_prefix_append_to(p, a, b);
b = slice_append(a, b, " is in ", 7);
b = netip_prefix_append_to(netip_prefix_masked(p), a, b);
printf("%.*s\n", (int)b.len, (const char *)b.p);
```

That prints:

```
2001:db8::1
2001:0db8:0000:0000:0000:0000:0000:0001
10.1.2.3/8 is in 10.0.0.0/8
```

`netip_prefix_masked` clears the host bits, so `10.1.2.3/8` becomes `10.0.0.0/8`. A prefix keeps the address it was parsed from, as in Go, and `netip_prefix_contains` only looks at the network bits either way.

Text and binary marshalling are Go's `MarshalText`, `UnmarshalText`, `MarshalBinary` and `UnmarshalBinary`, with `append_text` and `append_binary` alongside them.

## Walking a range

`netip_addr_next` and `netip_addr_prev` step one address at a time and give back the zero address when they run off the end, so a loop over a small prefix is short:

<!-- example: ../examples/net/netip.c#walk -->
```c
NetipPrefix p = netip_must_parse_prefix(BURROW_S("10.0.0.252/30"));
for (NetipAddr ip = netip_prefix_addr(p); netip_prefix_contains(p, ip);
     ip = netip_addr_next(ip))
    fmt_printf_v("%v\n", BURROW_ANY(TYPE_NETIP_ADDR, &ip));
```

That prints:

```
10.0.0.252
10.0.0.253
10.0.0.254
10.0.0.255
```

## Zones

A zone such as `eth0` in `fe80::1%eth0` goes through `unique_make`, so each distinct zone string is kept for as long as the process runs. Zones are interface names in practice and there are only a few of them. Do not parse addresses with zones out of untrusted input by the million.

## Speed

Parsing is faster than Go on every input in Go's own benchmarks, by 1.3 to 1.5 times. Printing to a new heap string is faster for the long IPv6 forms and a few nanoseconds slower for the short ones, IPv4 and prefixes among them, where most of the time is the allocation. Take the string from an arena, or use the `append_to` functions with a buffer you already have, and printing is ahead of Go on every input. The numbers are in the pull request that added the package and in [burrow-bench](https://github.com/tamnd/burrow-bench).

## net.IP, IPMask and IPNet

Go's `net` package has older address types that came before `net/netip`, and a lot of APIs still use them, `crypto/x509` among them. `burrow/net.h` has them too. `NetIP` and `NetIPMask` are byte slices, 4 or 16 bytes long, the same as in Go, and `NetIPNet` is an address and a mask. They are slices, so they allocate, and every function that makes one takes an allocator:

<!-- example: ../examples/net/ip.c#ip -->
```c
NetIP ip = net_parse_ip(a, BURROW_S("192.0.2.77"));
printf("%d %d\n", (int)ip.len, (int)net_ip_to4(ip).len); /* 16 4 */

Error err;
NetIPNet *lan;
NetIP host = net_parse_cidr(a, BURROW_S("10.1.2.3/20"), &lan, &err);
print(net_ip_string(host, a));
print(net_ip_net_string(lan, a));
printf("%d %d\n", net_ip_net_contains(lan, net_ipv4(a, 10, 1, 15, 1)),
       net_ip_net_contains(lan, ip));

NetIP v6 = net_parse_ip(a, BURROW_S("2001:db8:0:0:1::1"));
print(net_ip_string(net_ip_mask(v6, a, net_cidr_mask(a, 64, 128)), a));

net_parse_cidr(a, BURROW_S("10.1.2.3/33"), NULL, &err);
fmt_printf_v("%v\n", err);
```

That prints:

```
16 4
10.1.2.3
10.1.0.0/20
1 0
2001:db8::
invalid CIDR address: 10.1.2.3/33
```

`net_parse_ip` always gives 16 bytes, and `net_ip_to4` gives the 4-byte view of an IPv4 address, or the nil slice for one that is not. A nil `NetIP` is what Go's `nil` is, and its string is `<nil>`. Use `net_ip_equal` to compare two addresses, since an IPv4 address in 4 bytes and the same one in 16 are equal. The error from `net_parse_cidr` is a `NetParseError`, which `errors_as` finds with `TYPE_NET_PARSE_ERROR`.

`net_split_host_port` and `net_join_host_port` go between `host:port` strings and their parts, with the brackets an IPv6 literal needs:

<!-- example: ../examples/net/ip.c#hostport -->
```c
Error err;
Str port;
Str host = net_split_host_port(BURROW_S("[fe80::1%eth0]:8080"), &port, &err);
printf("%.*s %.*s\n", (int)host.len, host.p, (int)port.len, port.p);
print(net_join_host_port(a, host, BURROW_S("443")));
print(net_join_host_port(a, BURROW_S("example.com"), BURROW_S("https")));

net_split_host_port(BURROW_S("example.com"), &port, &err);
fmt_printf_v("%v\n", err);
```

That prints:

```
fe80::1%eth0 8080
[fe80::1%eth0]:443
example.com:https
address example.com: missing port in address
```

The host and port from `net_split_host_port` point into the string you pass, so nothing is allocated. A malformed string is a `NetAddrError` with Go's message. New code that does not have to match an older API is better off with `burrow/net/netip.h`, which never allocates.

## Connections and Pipe

`net.Conn`, `net.Addr` and `net.Listener` are interfaces in Go, and here they are the vtable and data pairs `NetConn`, `NetAddr` and `NetListener`, built the way [interfaces](interfaces.md) describes. A `NetConn` is a reader, a writer and a closer with the addresses and deadlines on top, and `net_conn_as_io_reader` and its siblings hand it to anything in `burrow/io.h` that wants one of those. No call allocates.

`net_pipe` is Go's `net.Pipe`: two ends of a connection that lives in memory, with no buffer in between, so each write waits until the other end has read all of it. It is handy for testing code that talks over a connection, and it is what `crypto/tls` is tested over:

<!-- example: ../examples/net/pipe.c#pipe -->
```c
NetConn client, server;
net_pipe(heap_allocator(), &client, &server);
SyncWaitGroup wg = {0};
sync_wait_group_go(&wg, BURROW_FN(Func, echo, &server));

Error err;
Byte buf[5];
Slice b = slice_from(buf, 5, 5, TYPE_BYTE);
io_write_string(net_conn_as_io_writer(client), BURROW_S("hello"), &err);
io_read_full(net_conn_as_io_reader(client), b, &err);
printf("%.*s\n", 5, (const char *)buf);

/* Nothing more is coming, so this read gives up at the deadline. */
client.vt->set_read_deadline(client.data,
                             time_add(time_now(), 10 * TIME_MILLISECOND));
io_read_full(net_conn_as_io_reader(client), b, &err);
Str msg = error_text(err);
printf("%.*s, timeout %d\n", (int)msg.len, (const char *)msg.p,
       net_error_timeout(err));

client.vt->closer.close(client.data);
sync_wait_group_wait(&wg);
net_pipe_free(client);
```

That prints:

```
hello
read pipe: i/o timeout, timeout 1
```

A call that runs past a deadline fails with a `NetOpError` that wraps `os_err_deadline_exceeded`, and `net_error_timeout` says true for it, the same as in Go. `errors_is` finds the wrapped error, and `errors_as` with `TYPE_NET_OP_ERROR` finds the `NetOpError`. Setting a deadline starts a timer, so it has to happen on a goroutine, which is why the example runs under `runtime_main`. Closing an end makes its own calls fail with `io_err_closed_pipe` and makes a read on the other end give `io_eof`.

Go's collector takes care of a pipe once nothing refers to it. Here `net_pipe_free` gives both ends back, given either one, once both are closed and no goroutine is still in a call on them. It stops any deadline timer that has not gone off yet.

## TCP

`net_listen_tcp` and `net_dial_tcp` are Go's `ListenTCP` and `DialTCP`, and the `NetTCPConn` and `NetTCPListener` they give back have Go's methods as functions. These take `NetTCPAddr` values, and `net_dial` and `net_listen`, under [Dial and Listen](#dial-and-listen), take the address as text. A listener on port 0 gets a free port from the system, and `net_tcp_listener_addr` says which:

<!-- example: ../examples/net/tcp.c#tcp -->
```c
Byte loopback[4] = {127, 0, 0, 1};
NetTCPAddr laddr = {slice_from(loopback, 4, 4, TYPE_BYTE), 0, BURROW_STR_EMPTY};
Error err;
NetTCPListener *l = net_listen_tcp(heap_allocator(), BURROW_S("tcp"), &laddr, &err);
if (l == NULL)
    return;
SyncWaitGroup wg = {0};
sync_wait_group_go(&wg, BURROW_FN(Func, serve, l));

/* Port 0 asked the system for a free port, and the address says which. */
const NetTCPAddr *bound = net_tcp_listener_addr(l).data;
NetTCPAddr raddr = {laddr.ip, bound->port, BURROW_STR_EMPTY};
NetTCPConn *c = net_dial_tcp(heap_allocator(), BURROW_S("tcp"), NULL, &raddr, &err);
if (c == NULL)
    return;
NetConn conn = net_tcp_conn_as_conn(c);
io_write_string(net_conn_as_io_writer(conn), BURROW_S("hello"), &err);
net_tcp_conn_close_write(c);

Byte buf[16];
Int n = io_read_full(net_conn_as_io_reader(conn), slice_from(buf, 5, 5, TYPE_BYTE),
                     &err);
printf("%.*s\n", (int)n, (const char *)buf);
const NetTCPAddr *remote = net_tcp_conn_remote_addr(c).data;
printf("same port: %d\n", remote->port == bound->port);

sync_wait_group_wait(&wg);
net_tcp_conn_free(c);
net_tcp_listener_free(l);

/* Errors read the way Go's do. */
raddr.port = 80;
if (net_dial_tcp(heap_allocator(), BURROW_S("tcp5"), NULL, &raddr, &err) == NULL) {
    Str msg = error_text(err);
    printf("%.*s\n", (int)msg.len, (const char *)msg.p);
}
```

That prints:

```
hello
same port: 1
dial tcp5 127.0.0.1:80: unknown network tcp5
```

Every call that can wait, from a dial to a read, has to be made on a goroutine. A goroutine waiting on a socket parks in the netpoller and holds a stack but no thread, so a server can have a goroutine per connection the way a Go server does. A Close from another goroutine wakes a read, a write or an accept with `net_err_closed`, and a deadline wakes it with `os_err_deadline_exceeded`.

The errors are Go's, down to the text. A failed call gives a `NetOpError` naming the operation and the addresses, wrapping an `OsSyscallError` naming the system call, wrapping the `Errno`, so "dial tcp 127.0.0.1:9: connect: connection refused" reads the same as it would from Go, and `errors_as` finds each layer. The one exception is the end of a stream, which is `io_eof` as it is, so `errors_is(err, io_eof)` works on a read.

`NetBuffers` is Go's `net.Buffers`, a slice of byte slices to write as one. Written to a TCP, UDP, IP or Unix connection with `net_buffers_write_to`, it goes in as few `writev` calls as the system allows, which is one for up to 1024 buffers, or one `WSASend` on Windows. Written to anything else, it is one `Write` per buffer. Either way the buffers that went are taken off the front, so a write that fails part way leaves what is still to go:

<!-- example: ../examples/net/buffers.c#buffers -->
```c
char one[] = "first line\n";
char two[] = "second line\n";
char three[] = "third line\n";
Slice parts[3] = {
    slice_from(one, (Int)sizeof one - 1, (Int)sizeof one - 1, TYPE_BYTE),
    slice_from(two, (Int)sizeof two - 1, (Int)sizeof two - 1, TYPE_BYTE),
    slice_from(three, (Int)sizeof three - 1, (Int)sizeof three - 1, TYPE_BYTE),
};
NetBuffers v = slice_from(parts, 3, 3, TYPE_BYTES);

/* One writev for all three, since the writer is a connection. */
NetConn conn = net_tcp_conn_as_conn(c);
int64_t n = net_buffers_write_to(&v, net_conn_as_io_writer(conn), &err);
printf("wrote %d bytes, %d buffers left\n", (int)n, (int)v.len);
```

With the example's server printing what it reads, that prints:

```
wrote 34 bytes, 0 buffers left
server got 34 bytes: first line
second line
third line
```

`net_tcp_conn_read_from` and `net_tcp_conn_write_to` are the connection's `ReadFrom` and `WriteTo`, so `io_copy` to or from a TCP connection goes through them, as it does in Go. They copy through a buffer on every system, where Go on Linux would splice or sendfile, and an error from either comes in a `NetOpError` named "readfrom" or "writeto" around the read or write that failed.

A dialed connection, like an accepted one, has Nagle's algorithm off and keep-alives on, with Go's defaults of 15 seconds idle, 15 seconds between probes and 9 probes. `net_tcp_conn_set_keep_alive_config` and the other setters change them. `net_tcp_conn_free` and `net_tcp_listener_free` close what is still open and give the memory back, and the addresses a connection hands out belong to it until then.

On Windows a socket is read by handing the kernel the read and waiting for it to finish, which is a different poller from the one here, so until that arrives a dial or a listen there fails with `ENOSYS`. wasip1 has no sockets to dial with.

## UDP

`net_listen_udp` and `net_dial_udp` are Go's `ListenUDP` and `DialUDP`. A socket from `net_listen_udp` is bound but not connected, so it can hear from anyone and says where each datagram goes with one of the write-to functions. A socket from `net_dial_udp` is connected to one peer, writes with `net_udp_conn_write` and only hears from that peer:

<!-- example: ../examples/net/udp.c#udp -->
```c
Byte loopback[4] = {127, 0, 0, 1};
NetUDPAddr laddr = {slice_from(loopback, 4, 4, TYPE_BYTE), 0, BURROW_STR_EMPTY};
Error err;
NetUDPConn *server =
    net_listen_udp(heap_allocator(), BURROW_S("udp"), &laddr, &err);
NetUDPConn *client =
    net_listen_udp(heap_allocator(), BURROW_S("udp"), &laddr, &err);
if (server == NULL || client == NULL)
    return;

/* Neither socket is connected, so each datagram says where it goes. */
NetipAddrPort to = net_udp_addr_addr_port(net_udp_conn_local_addr(server).data);
char ping[] = "ping";
net_udp_conn_write_to_udp_addr_port(client, slice_from(ping, 4, 4, TYPE_BYTE), to,
                                    &err);

Byte buf[64];
NetipAddrPort from;
Int n = net_udp_conn_read_from_udp_addr_port(
    server, slice_from(buf, (Int)sizeof buf, (Int)sizeof buf, TYPE_BYTE), &from,
    &err);
Str ip = netip_addr_string(netip_addr_port_addr(from), heap_allocator());
printf("%.*s from %.*s\n", (int)n, (const char *)buf, (int)ip.len,
       (const char *)ip.p);
const NetUDPAddr *client_addr = net_udp_conn_local_addr(client).data;
printf("same port: %d\n", netip_addr_port_port(from) == client_addr->port);
mem_free(heap_allocator(), (void *)(uintptr_t)ip.p, (size_t)ip.len, 1);

net_udp_conn_free(client);
net_udp_conn_free(server);

/* Errors read the way Go's do. */
laddr.port = 53;
if (net_dial_udp(heap_allocator(), BURROW_S("udp5"), NULL, &laddr, &err) == NULL) {
    Str msg = error_text(err);
    printf("%.*s\n", (int)msg.len, (const char *)msg.p);
}
```

That prints:

```
ping from 127.0.0.1
same port: 1
dial udp5 127.0.0.1:53: unknown network udp5
```

Go has three ways to say who sent a datagram, and so does this. `net_udp_conn_read_from_udp_addr_port` fills in a `NetipAddrPort` and needs no memory, which makes it the one to use in a loop. `net_udp_conn_read_from_udp` makes a `NetUDPAddr` in the allocator it is given, and `net_udp_conn_read_from` gives the same address as a `NetAddr`. The addresses those two make belong to the caller and go back with `net_udp_addr_free`.

A datagram is read whole. When it is longer than the buffer, the rest of it is lost and the next read starts on the next datagram, as it does in Go. Writing to a connected socket with a write-to function fails with `net_err_write_to_connected`, and the errors otherwise are Go's, down to "write udp 127.0.0.1:5000->127.0.0.1:70000: sendto: invalid argument" for a port that does not fit.

`net_udp_conn_read_msg_udp` and `net_udp_conn_write_msg_udp` carry control messages alongside the data, in the system's own layout, which `syscall_parse_socket_control_message` picks apart, and they have `_addr_port` twins that need no memory for the address. `ListenMulticastUDP` is under [Interfaces](#interfaces), since it takes one. Windows and wasip1 are where TCP is: a dial or a listen on Windows fails with `ENOSYS` for now, and wasip1 has no sockets.

## Unix

`net_listen_unix`, `net_listen_unixgram` and `net_dial_unix` are Go's `ListenUnix`, `ListenUnixgram` and `DialUnix`. The networks are the three Go has: "unix" for streams, "unixgram" for datagrams and "unixpacket" for sequenced packets, which keep each write apart the way datagrams do but are connected the way streams are. A `NetUnixAddr` is a name and a network, and the name is a path:

<!-- example: ../examples/net/unix.c#unix -->
```c
Error err;
Str dir = os_mkdir_temp(heap_allocator(), BURROW_STR_EMPTY, BURROW_S("ex"), &err);
if (BURROW_FAILED(err))
    return;
Str path = filepath_join_v(heap_allocator(), 2, dir, BURROW_S("echo.sock"));
NetUnixAddr addr = {path, BURROW_S("unix")};
NetUnixListener *l =
    net_listen_unix(heap_allocator(), BURROW_S("unix"), &addr, &err);
if (l == NULL)
    return;

NetUnixConn *client =
    net_dial_unix(heap_allocator(), BURROW_S("unix"), NULL, &addr, &err);
NetUnixConn *server = net_unix_listener_accept_unix(l, &err);
if (client == NULL || server == NULL)
    return;

char hello[] = "hello";
(void)net_unix_conn_write(client, slice_from(hello, 5, 5, TYPE_BYTE), &err);
Byte buf[64];
Int n = net_unix_conn_read(
    server, slice_from(buf, (Int)sizeof buf, (Int)sizeof buf, TYPE_BYTE), &err);
printf("%.*s\n", (int)n, (const char *)buf);

/* The server's end is named for the path the listener is on. */
const NetUnixAddr *local = net_unix_conn_local_addr(server).data;
printf("same path: %d\n", str_eq(local->name, path));

net_unix_conn_free(client);
net_unix_conn_free(server);

/* Closing a listener removes the file it made, as Go's does. */
net_unix_listener_free(l);
(void)os_lstat(heap_allocator(), path, &err);
printf("gone: %d\n", os_is_not_exist(err));
(void)os_remove_all(dir);
mem_free(heap_allocator(), (void *)(uintptr_t)path.p, (size_t)path.len, 1);
mem_free(heap_allocator(), (void *)(uintptr_t)dir.p, (size_t)dir.len, 1);

/* Errors read the way Go's do. */
NetUnixAddr there = {BURROW_S("/run/app.sock"), BURROW_S("unix")};
if (net_listen_unix(heap_allocator(), BURROW_S("unixgram"), &there, &err) == NULL) {
    Str msg = error_text(err);
    printf("%.*s\n", (int)msg.len, (const char *)msg.p);
}
```

That prints:

```
hello
same path: 1
gone: 1
listen unixgram /run/app.sock: unknown network unixgram
```

A listener made by `net_listen_unix` removes its file when it closes, and so does `net_unix_listener_free`. `net_unix_listener_set_unlink_on_close` turns that off, for a socket file that should outlive the program. A socket from `net_listen_unixgram` never removes its file, which is what Go does too.

On Linux a name that starts with "@" is in the abstract namespace, has no file, and goes away with the last socket on it. Listening on an empty name binds to a fresh abstract name, and a socket that was never bound is called "@" there and "" everywhere else, so those are the names a dialer's end and an unbound sender have. A name has to fit in the system's sockaddr, which is 107 bytes on Linux and 103 on macOS and the BSDs, and a longer one fails with "bind: invalid argument" as it does in Go.

The datagram functions are the UDP ones with a `NetUnixAddr` in place of a `NetUDPAddr`: `net_unix_conn_read_from_unix` makes the sender's address in the allocator it is given, and `net_unix_conn_write_to_unix` sends to a name. `net_unix_conn_read_msg_unix` and `net_unix_conn_write_msg_unix` pass descriptors between processes, with `syscall_unix_rights` making the control message and `syscall_parse_unix_rights` reading it back. The descriptors that arrive are close-on-exec, as in Go. This one hands an open file across a connection:

<!-- example: ../examples/net/rights.c#rights -->
```c
Arena ar;
arena_init(&ar, NULL, 0);
Alloc *a = arena_allocator(&ar);
Error err;
Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("ex"), &err);
if (BURROW_FAILED(err))
    return;
Str note = filepath_join_v(a, 2, dir, BURROW_S("note.txt"));
char text[] = "read through a passed descriptor";
(void)os_write_file(note, slice_from(text, 32, 32, TYPE_BYTE), 0600);

NetUnixAddr addr = {filepath_join_v(a, 2, dir, BURROW_S("pass.sock")),
                    BURROW_S("unix")};
NetUnixListener *l = net_listen_unix(a, BURROW_S("unix"), &addr, &err);
NetUnixConn *client = net_dial_unix(a, BURROW_S("unix"), NULL, &addr, &err);
NetUnixConn *server = net_unix_listener_accept_unix(l, &err);
if (client == NULL || server == NULL)
    return;

/* One end opens the file and sends the descriptor along with a byte. */
Int fd = syscall_open(note, SYSCALL_O_RDONLY, 0, &err);
Slice rights = syscall_unix_rights(a, (Slice){&fd, 1, 1, TYPE_INT});
char one[] = "f";
Int oobn = 0;
(void)net_unix_conn_write_msg_unix(client, slice_from(one, 1, 1, TYPE_BYTE), rights,
                                   NULL, &oobn, &err);
printf("sent all of it: %d\n", oobn == rights.len);
(void)syscall_close(fd);

/* The other end gets a descriptor of its own for the same open file. */
Byte b[1];
Byte oob[64];
(void)net_unix_conn_read_msg_unix(server, slice_from(b, 1, 1, TYPE_BYTE),
                                  slice_from(oob, 64, 64, TYPE_BYTE), a, &oobn,
                                  NULL, NULL, &err);
Slice msgs = syscall_parse_socket_control_message(
    a, slice_from(oob, oobn, oobn, TYPE_BYTE), &err);
Slice fds =
    syscall_parse_unix_rights(a, &((SyscallSocketControlMessage *)msgs.p)[0], &err);
if (BURROW_FAILED(err) || fds.len != 1)
    return;
Int got = ((const Int *)fds.p)[0];
Byte data[64];
Int n = syscall_read(got, slice_from(data, 64, 64, TYPE_BYTE), &err);
printf("%.*s\n", (int)n, (const char *)data);
(void)syscall_close(got);

net_unix_conn_free(client);
net_unix_conn_free(server);
net_unix_listener_free(l);
(void)os_remove_all(dir);
arena_free(&ar);
```

That prints:

```
sent all of it: 1
read through a passed descriptor
```

The `File` methods are still to come, and Windows is where TCP is for now.

## Raw IP

`net_listen_ip` and `net_dial_ip` are Go's `ListenIP` and `DialIP`, which make raw IP sockets. The network is "ip", "ip4" or "ip6" with the protocol after a colon, by number or by the name /etc/protocols gives it, so "ip4:icmp" and "ip4:1" are the same thing. A `NetIPAddr` is an address and a zone with no port. The system only hands out raw sockets to root, or to a program with CAP_NET_RAW on Linux, and anyone else gets an error, which is EPERM on Linux. `net_dialer_dial_ip` is the Dialer's `DialIP`, new in Go 1.27, which takes `NetipAddr` values and a context. This one sends an ICMP echo request to the loopback address and waits for the reply:

<!-- example: ../examples/net/ipconn.c#ipconn -->
```c
Arena ar;
arena_init(&ar, NULL, 0);
Alloc *a = arena_allocator(&ar);
Error err;
/* Raw sockets need root, or CAP_NET_RAW on Linux. */
NetIPConn *c = net_listen_ip(a, BURROW_S("ip4:icmp"), NULL, &err);
if (c == NULL) {
    Str s = error_text(err);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
    arena_free(&ar);
    return;
}
(void)net_ip_conn_set_deadline(c, time_add(time_now(), 5 * TIME_SECOND));

/* An echo request: type 8, code 0, the checksum, an identifier and a
 * sequence number, then the data. */
Byte req[12] = {8, 0, 0, 0, 0x62, 0x78, 0, 1, 'p', 'i', 'n', 'g'};
icmp_checksum(req, sizeof req);
NetIPAddr *to =
    net_resolve_ip_addr(a, BURROW_S("ip4"), BURROW_S("127.0.0.1"), &err);
(void)net_ip_conn_write_to_ip(c, slice_from(req, sizeof req, sizeof req, TYPE_BYTE),
                              to, &err);

/* The socket sees every ICMP packet to this machine, so wait for the
 * reply, which is type 0. read_from_ip takes the IPv4 header off. */
for (;;) {
    Byte got[128];
    NetIPAddr *from = NULL;
    Int n = net_ip_conn_read_from_ip(
        c, slice_from(got, sizeof got, sizeof got, TYPE_BYTE), a, &from, &err);
    if (BURROW_FAILED(err)) {
        Str s = error_text(err);
        printf("%.*s\n", (int)s.len, (const char *)s.p);
        break;
    }
    if (n >= 8 && got[0] == 0 && memcmp(got + 4, req + 4, 4) == 0) {
        Str s = net_ip_addr_string(from, a);
        printf("echo reply from %.*s, %d bytes\n", (int)s.len, (const char *)s.p,
               (int)n);
        break;
    }
}
net_ip_conn_free(c);
arena_free(&ar);
```

On an IPv4 socket, `net_ip_conn_read_from_ip` and the `read_from` of the `NetPacketConn` take the IPv4 header off the front of each packet, as Go's `ReadFromIP` does, while `net_ip_conn_read` gives the packet as the system hands it over, header and all. `net_ip_conn_read_msg_ip` leaves the header where it is, as Go's `ReadMsgIP` does, and `net_ip_conn_write_msg_ip` always needs an address. Windows is where TCP is for now.

## Interfaces

`net_interfaces` is Go's `Interfaces`, the machine's network interfaces with their index, MTU, name, hardware address and flags, and `net_interface_by_index` and `net_interface_by_name` find one. `net_interface_addrs_of` is the `Addrs` method, the addresses of one interface, each a `NetAddr` that holds a `NetIPNet` with the prefix, and `net_interface_addrs` is the `InterfaceAddrs` function, every address on the machine. On Windows the list includes the anycast addresses too, as `NetIPAddr`, which is what Go gives there. `net_interface_multicast_addrs` is the groups an interface has joined. This one prints each interface and its addresses:

<!-- example: ../examples/net/interfaces.c#interfaces -->
```c
Arena ar;
arena_init(&ar, NULL, 0);
Alloc *a = arena_allocator(&ar);
Error err;
Slice ift = net_interfaces(a, &err);
if (BURROW_FAILED(err)) {
    fmt_printf_v("%v\n", err);
    arena_free(&ar);
    return;
}
for (Int i = 0; i < ift.len; i++) {
    const NetInterface *ifi = &((const NetInterface *)ift.p)[i];
    Str flags = net_flags_string(ifi->flags, a);
    Str hw = net_hardware_addr_string(ifi->hardware_addr, a);
    printf("%d %.*s mtu %d <%.*s> %.*s\n", (int)ifi->index, (int)ifi->name.len,
           (const char *)ifi->name.p, (int)ifi->mtu, (int)flags.len,
           (const char *)flags.p, (int)hw.len, (const char *)hw.p);

    /* Unicast addresses come back as NetIPNet, with the prefix. */
    Slice addrs = net_interface_addrs_of(ifi, a, &err);
    for (Int j = 0; j < addrs.len; j++) {
        NetAddr x = ((const NetAddr *)addrs.p)[j];
        Str s = x.vt->string(x.data, a);
        printf("    %.*s\n", (int)s.len, (const char *)s.p);
    }
}
arena_free(&ar);
```

On Linux the list comes from a netlink dump and the groups from /proc/net/igmp and /proc/net/igmp6, which is where Go reads them. On macOS and the BSDs it comes from `getifaddrs` and `getifmaddrs`, and on Windows from `GetAdaptersAddresses`. These are also what turns an IPv6 zone such as `eth0` into the index a socket needs, and back. Like Go, the names are cached and the cache is read again when it is more than a minute old or when a name or index is not in it.

`net_parse_mac` reads a hardware address in any of the three forms Go reads, with colons, with hyphens, or in dotted groups of four digits, for 48 bit and 64 bit addresses and 20 byte InfiniBand ones:

<!-- example: ../examples/net/mac.c#mac -->
```c
Error err;
NetHardwareAddr hw = net_parse_mac(a, BURROW_S("00-00-5E-00-53-01"), &err);
print(net_hardware_addr_string(hw, a));

/* Cisco's dotted form, and an EUI-64. */
hw = net_parse_mac(a, BURROW_S("0200.5e10.0000.0001"), &err);
printf("%d ", (int)hw.len);
print(net_hardware_addr_string(hw, a));

net_parse_mac(a, BURROW_S("01:02:03:04:05"), &err);
fmt_printf_v("%v\n", err);
```

### Multicast

`net_listen_multicast_udp` is Go's `ListenMulticastUDP`. It binds the group's port, joins the group on the interface it is given, and turns off the loopback of what the socket sends itself, as Go does. Other sockets can listen on the same group and port. With a NULL interface the system picks one, which is rarely the right one on a machine with several, so name it:

<!-- example: ../examples/net/multicast.c#multicast -->
```c
Arena ar;
arena_init(&ar, NULL, 0);
Alloc *a = arena_allocator(&ar);
Error err;

/* The loopback interface, which every machine has. */
const NetInterface *lo = NULL;
Slice ift = net_interfaces(a, &err);
for (Int i = 0; i < ift.len && lo == NULL; i++) {
    const NetInterface *ifi = &((const NetInterface *)ift.p)[i];
    if ((ifi->flags & NET_FLAG_LOOPBACK) != 0 && (ifi->flags & NET_FLAG_UP) != 0)
        lo = ifi;
}

/* 224.0.0.254 is a group set aside for experiments. */
NetUDPAddr group = {net_ipv4(a, 224, 0, 0, 254), 12345, BURROW_STR_EMPTY};
NetUDPConn *c =
    net_listen_multicast_udp(heap_allocator(), BURROW_S("udp4"), lo, &group, &err);
if (c == NULL) {
    fmt_printf_v("%v\n", err);
    arena_free(&ar);
    return;
}
NetAddr la = net_udp_conn_local_addr(c);
Str s = la.vt->string(la.data, a);
printf("listening on %.*s\n", (int)s.len, (const char *)s.p);

/* The group is in the interface's list of joined groups now. */
if (lo != NULL) {
    Slice groups = net_interface_multicast_addrs(lo, a, &err);
    for (Int i = 0; i < groups.len; i++) {
        NetAddr g = ((const NetAddr *)groups.p)[i];
        Str gs = g.vt->string(g.data, a);
        printf("    %.*s\n", (int)gs.len, (const char *)gs.p);
    }
}
net_udp_conn_free(c);
arena_free(&ar);
```

Linux joins by the interface's index. macOS, the BSDs and Windows join by one of the interface's IPv4 addresses, so there an interface with no IPv4 address fails with "no such multicast network interface", which is Go's error. Go points anything more involved at golang.org/x/net/ipv4 and ipv6, which burrow does not have yet.

## Sockets as files

Each connection and listener has Go's `File` method, `net_tcp_conn_file`, `net_tcp_listener_file` and the rest, which hands back a copy of the socket as an `OsFile`. The copy stays open when the connection closes, and the caller closes it. `net_file_conn`, `net_file_listener` and `net_file_packet_conn` go the other way, which is how a socket handed over by a parent process or a service manager becomes a connection again. What comes back depends on the socket's family and type, so a TCP socket becomes a TCPConn and a datagram Unix socket a UnixConn, and asking for a listener from a socket that is not listening gives EINVAL:

<!-- example: ../examples/net/file.c#file -->
```c
/* A copy of the listening socket as a file, which outlives the listener. */
NetAddr was = l.vt->addr(l.data);
Str before = was.vt->string(was.data, a);
OsFile *f =
    net_tcp_listener_file(net_listener_as_tcp_listener(l), heap_allocator(), &err);
net_listener_free(l);
if (f == NULL) {
    fmt_printf_v("%v\n", err);
    arena_free(&ar);
    return;
}

/* And a listener again, from the file, on the same address. */
NetListener l2 = net_file_listener(heap_allocator(), f, &err);
(void)os_file_close(f);
os_file_free(f);
if (l2.vt == NULL) {
    fmt_printf_v("%v\n", err);
    arena_free(&ar);
    return;
}
NetAddr now = l2.vt->addr(l2.data);
Str after = now.vt->string(now.data, a);
printf("same address: %s\n", str_eq(before, after) ? "yes" : "no");

SyncWaitGroup wg = {0};
sync_wait_group_go(&wg, BURROW_FN(Func, dial, &now));
NetConn c = l2.vt->accept(l2.data, &err);
if (c.vt != NULL) {
    NetAddr ra = c.vt->remote_addr(c.data);
    Str network = ra.vt->network(ra.data);
    printf("accepted a %.*s connection\n", (int)network.len,
           (const char *)network.p);
    net_conn_free(c);
}
sync_wait_group_wait(&wg);
net_listener_free(l2);
```

That prints:

```
same address: yes
accepted a tcp connection
```

The file keeps its copy in non blocking mode until something asks for its descriptor with `os_file_fd`, which puts it in blocking mode first, as Go's `Fd` does, since code that takes a descriptor from a connection's file has always had a blocking one from Go. The connection made from a file gets a copy of its own, in non blocking mode for the poller, and leaves the file's alone. A Unix listener made from a file does not remove its socket file when it closes. On Windows the copy comes from `WSADuplicateSocket` and is never put in non blocking mode. WASI cannot copy a socket yet, so these give ENOSYS there.

## Dial and Listen

`net_dial` and `net_listen` are Go's `Dial` and `Listen`. They take the network and the address as text, look up a host name and a service name for the port, and give back a `NetConn` or a `NetListener` whatever the network. `NetDialer` and `NetListenConfig` are the structs behind them, with Go's fields: a timeout, a deadline, a local address, the keep-alive settings, a resolver, and a `control` callback that sees the socket before it connects. The zero value of either works, and so does passing NULL.

<!-- example: ../examples/net/dial.c#dial -->
```c
/* Connections go in the heap, since the server's goroutine makes one
 * too. */
Alloc *heap = heap_allocator();
Error err;
NetListener l = net_listen(heap, BURROW_S("tcp"), BURROW_S("127.0.0.1:0"), &err);
if (l.vt == NULL)
    return;
SyncWaitGroup wg = {0};
sync_wait_group_go(&wg, BURROW_FN(Func, serve, &l));

/* The text this goroutine makes goes in an arena. The listener's
 * address as text is something Dial takes. */
Arena ar;
arena_init(&ar, NULL, 0);
Alloc *a = arena_allocator(&ar);
NetAddr la = l.vt->addr(l.data);
Str address = la.vt->string(la.data, a);
NetDialer d = {0};
d.timeout = 5 * TIME_SECOND;
NetConn c = net_dialer_dial(&d, heap, BURROW_S("tcp"), address, &err);
if (c.vt != NULL) {
    Slice got = io_read_all(a, net_conn_as_io_reader(c), &err);
    printf("%.*s\n", (int)got.len, (const char *)got.p);
    net_conn_free(c);
}
/* Closing the listener stops an accept that is still waiting. */
(void)l.vt->closer.close(l.data);
sync_wait_group_wait(&wg);
net_listener_free(l);

/* Ports by name, and zones, the way ResolveTCPAddr reads them. */
NetTCPAddr *ta =
    net_resolve_tcp_addr(a, BURROW_S("tcp"), BURROW_S("[::1%lo]:http"), &err);
if (ta != NULL) {
    printf("port %d zone %.*s\n", (int)ta->port, (int)ta->zone.len,
           (const char *)ta->zone.p);
    print_str(net_tcp_addr_string(ta, a));
}

/* A dial that cannot start says why, as an OpError. */
c = net_dial(heap, BURROW_S("tcp6"), BURROW_S("127.0.0.1:80"), &err);
if (c.vt == NULL)
    print_str(error_text(err));
arena_free(&ar);
```

That prints:

```
hello from the server
port 80 zone lo
[::1%lo]:80
dial tcp6: address 127.0.0.1: no suitable address found
```

`net_listen_packet` is Go's `ListenPacket`, for "udp", "udp4", "udp6", "unixgram" and the "ip" networks with a protocol, such as "ip4:icmp". It gives back a `NetPacketConn`, whose `read_from` says who sent each packet and whose `write_to` sends one to any address. `net_udp_conn_as_packet_conn`, `net_unix_conn_as_packet_conn` and `net_ip_conn_as_packet_conn` turn the connections the UDP, Unix and IP sections make into one, and `net_packet_conn_free` gives any of them back.

<!-- example: ../examples/net/listen_packet.c#listen-packet -->
```c
Alloc *heap = heap_allocator();
Arena ar;
arena_init(&ar, NULL, 0);
Alloc *a = arena_allocator(&ar);
Error err;
NetPacketConn pc =
    net_listen_packet(heap, BURROW_S("udp"), BURROW_S("127.0.0.1:0"), &err);
if (pc.vt == NULL) {
    arena_free(&ar);
    return;
}
NetAddr la = pc.vt->local_addr(pc.data);
NetConn c = net_dial(heap, BURROW_S("udp"), la.vt->string(la.data, a), &err);
if (c.vt != NULL) {
    /* One datagram there, and the answer back to whoever sent it. */
    char buf[64];
    char ping[] = "ping";
    (void)c.vt->writer.write(c.data, slice_from(ping, 4, 4, TYPE_BYTE), &err);
    NetAddr from = {NULL, NULL};
    Int n = pc.vt->read_from(pc.data,
                             slice_from(buf, sizeof buf, sizeof buf, TYPE_BYTE), a,
                             &from, &err);
    NetAddr cl = c.vt->local_addr(c.data);
    bool same = from.vt != NULL &&
                str_eq(from.vt->string(from.data, a), cl.vt->string(cl.data, a));
    printf("got %.*s, from the dialer: %s\n", (int)n, buf, same ? "true" : "false");
    char pong[] = "pong";
    (void)pc.vt->write_to(pc.data, slice_from(pong, 4, 4, TYPE_BYTE), from, &err);
    n = c.vt->reader.read(c.data,
                          slice_from(buf, sizeof buf, sizeof buf, TYPE_BYTE), &err);
    printf("got %.*s back\n", (int)n, buf);
    net_conn_free(c);
}
net_packet_conn_free(pc);

/* Only the datagram networks make a packet connection. */
pc = net_listen_packet(heap, BURROW_S("tcp"), BURROW_S("127.0.0.1:0"), &err);
if (pc.vt == NULL) {
    Str s = error_text(err);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}
arena_free(&ar);
```

That prints:

```
got ping, from the dialer: true
got pong back
listen tcp 127.0.0.1:0: address 127.0.0.1:0: unexpected address type
```

A name with several addresses is tried one address at a time, each with its share of whatever time is left, and for "tcp" a name with both IPv4 and IPv6 addresses races the first of each, starting the second family 300ms after the first unless `fallback_delay` says otherwise. That is RFC 6555, which Go calls Happy Eyeballs. The connection that loses the race is closed before the dial returns. `net_dialer_dial_context` takes a context, and cancelling it stops a dial that is still going.

`net_conn_free` and `net_listener_free` close what they are given and give it back, and `net_conn_as_tcp_conn` and its siblings get the concrete type when you need its methods. `net_resolve_tcp_addr`, `net_resolve_udp_addr` and `net_resolve_ip_addr` are Go's `ResolveTCPAddr` and friends, for when you want the address without the connection.

When a dial fails, the error is a `NetOpError` with the op "dial", and it names the addresses it tried. Those addresses live in the error arena of the goroutine that dialed, so the error stays readable for as long as the goroutine keeps it, and a dial that works leaves nothing behind there.

### Multipath TCP

Multipath TCP lets one connection use several paths at once, such as Wi-Fi and a mobile link, and only Linux has it. As in Go, a TCP listener asks for it unless told otherwise and a dial does not. `net_listen_config_set_multipath_tcp` and `net_dialer_set_multipath_tcp` change that, and `net_listen_config_multipath_tcp` and `net_dialer_multipath_tcp` say what was asked for. Left alone, both follow the GODEBUG environment variable as Go reads it: multipathtcp=0 turns it off for listeners, 1 turns it on for dials, and 3 turns it on for dials and off for listeners. A socket that cannot have Multipath TCP, because the kernel is too old or it is turned off, falls back to plain TCP without an error, and a peer that does not speak it gets plain TCP too, so `net_tcp_conn_multipath_tcp` is how to tell what a connection ended up with. On anything but Linux it always says no.

## Looking up names

`net_lookup_host`, `net_lookup_ip`, `net_lookup_port`, `net_lookup_addr` and the record lookups are Go's, and so is the choice of who answers them. There are two resolvers. One is Go's own, which reads `/etc/hosts`, `/etc/resolv.conf` and `/etc/nsswitch.conf` and talks to the name servers itself. The other is the system's `getaddrinfo` and `getnameinfo`, which Go reaches through cgo and burrow reaches through its platform layer. A lookup goes to the system when the configuration has something Go's resolver does not understand, such as an NSS module or mDNS in `nsswitch.conf`, and on macOS, where the system is always asked. `GODEBUG=netdns=go` and `GODEBUG=netdns=cgo` force one or the other, `GODEBUG=netdns=1` says which was picked and why, and a `NetResolver` with `prefer_go` set gets Go's.

<!-- example: ../examples/net/lookup.c#lookup -->
```c
Arena ar;
arena_init(&ar, NULL, 0);
Alloc *a = arena_allocator(&ar);
Error err;

/* A service by name. The system is asked first where there is one, and
 * Go's own table answers when it does not know. */
Int port = net_lookup_port(BURROW_S("tcp"), BURROW_S("https"), &err);
printf("https is port %d\n", (int)port);

/* A host, through whichever resolver the system's configuration and
 * GODEBUG=netdns= pick. */
Slice addrs = net_lookup_host(a, BURROW_S("localhost"), &err);
printf("localhost has 127.0.0.1: %s\n", has(addrs, "127.0.0.1") ? "yes" : "no");

/* The same through Go's resolver, which reads the hosts file itself. */
NetResolver r = {0};
r.prefer_go = true;
addrs = net_resolver_lookup_host(&r, a, context_background(), BURROW_S("localhost"),
                                 &err);
printf("and so says Go's: %s\n", has(addrs, "127.0.0.1") ? "yes" : "no");
arena_free(&ar);
```

That prints:

```
https is port 443
localhost has 127.0.0.1: yes
and so says Go's: yes
```

The system's calls block the thread that makes them, and burrow has no way to hand a goroutine's thread to another one while that happens, the way cgo does. So the calls run on threads of their own, which wait around for a while for the next lookup and then exit, and the goroutine waits for the answer as it would for a socket. A context that is done stops the wait but not the call, which finishes on its own thread and is thrown away. At most 500 calls run at once, fewer when the limit on open files is low, which is Go's rule.

Windows and wasip1 always use Go's resolver for now. wasip1 has no other, and Go's Windows lookups go through `GetAddrInfoW` and `DnsQuery_W` in a way of their own that is still to come.

## URLs

`url_parse` splits a URL into a `Url` with the same fields as Go's `url.URL`. `path` holds the decoded path and `url_escaped_path` gives back the form that goes on the wire. The parse makes one allocation that holds the `Url` and every string in it, so the input can go away while the `Url` lives, and `url_free` gives it back. With an arena you do not need to free at all. In these examples `P(s)` is short for `(int)(s).len, (const char *)(s).p`, the two arguments that `%.*s` wants:

<!-- example: ../examples/net/url.c#parse -->
```c
Error err;
Url *u = url_parse(
    a, BURROW_S("https://ann:secret@go.dev:8443/doc/a%20b?q=url&m=text#top"), &err);
printf("%.*s | %.*s | %.*s | %.*s\n", P(u->scheme), P(url_hostname(u)),
       P(url_port(u)), P(u->path));
printf("%.*s | %.*s | %.*s\n", P(url_escaped_path(u, a)), P(u->raw_query),
       P(u->fragment));
printf("%.*s\n", P(url_redacted(u, a)));
url_parse(a, BURROW_S("http://x/%zz"), &err);
fmt_printf_v("%v\n", err);
```

That prints:

```
https | go.dev | 8443 | /doc/a b
/doc/a%20b | q=url&m=text | top
https://ann:xxxxx@go.dev:8443/doc/a%20b?q=url&m=text#top
parse "http://x/%zz": invalid URL escape "%zz"
```

The error is a `UrlError` with Go's `Op`, `URL` and `Err`, and its text is Go's word for word. `errors_as` with `TYPE_URL_ERROR`, `TYPE_URL_ESCAPE_ERROR` or `TYPE_URL_INVALID_HOST_ERROR` gets at the parts.

## Relative references

`url_parse_ref` is Go's `u.Parse`: it parses a reference and resolves it against `u` the way a browser follows a link. `url_resolve_reference` does the same with a `Url` you already have. `url_join_path` appends path elements and cleans the result, and `url_join_path_v` takes them as arguments:

<!-- example: ../examples/net/url.c#resolve -->
```c
Url *base = url_parse(a, BURROW_S("https://go.dev/doc/effective_go"), NULL);
Url *next = url_parse_ref(base, a, BURROW_S("../blog/?page=2"), NULL);
printf("%.*s\n", P(url_string(next, a)));
Url *joined = url_join_path_v(base, a, 2, BURROW_S("../ref"), BURROW_S("spec/"));
printf("%.*s\n", P(url_string(joined, a)));
```

That prints:

```
https://go.dev/blog/?page=2
https://go.dev/doc/ref/spec/
```

## Query strings

A `UrlValues` is a `Map` from each key to a slice of values, which is Go's `url.Values`. `url_query` parses the query of a `Url`, `url_values_get` returns the first value for a key, and `url_values_encode` writes them back out sorted by key:

<!-- example: ../examples/net/url.c#query -->
```c
Url *u = url_parse(a, BURROW_S("/search?q=c+library&tag=go&tag=c"), NULL);
UrlValues q = url_query(u, a);
Slice tags = url_values_get_all(q, BURROW_S("tag"));
printf("%.*s, %d tags\n", P(url_values_get(q, BURROW_S("q"))), (int)tags.len);
url_values_set(q, BURROW_S("page"), BURROW_S("2"));
url_values_del(q, BURROW_S("tag"));
printf("%.*s\n", P(url_values_encode(q, a)));
printf("%.*s\n", P(url_path_escape(a, BURROW_S("a b/c"))));
```

That prints:

```
c library, 2 tags
page=2&q=c+library
a%20b%2Fc
```

A `UrlValues` is many small allocations, and its strings may point into the query they came from, so an arena is the easy way to hold one. `url_query_escape` and `url_path_escape` return their input unchanged, with no allocation, when there is nothing to escape.

Two of Go's GODEBUG settings apply. `urlstrictcolons=0` lets an http or https host have more than one colon, and `urlmaxqueryparams=N` changes the cap of 10000 parameters that `url_parse_query` puts on a query, with 0 for no cap.

## Text protocols

`net/textproto` is the line-based request and response layer under HTTP, SMTP, NNTP and FTP, and it follows Go's `net/textproto`. A `TextprotoReader` sits on a `BufioReader` and reads lines, continued lines, numbered replies, dot-terminated blocks and MIME headers. `textproto_reader_read_mime_header` returns a `TextprotoMIMEHeader`, a `Map` from each canonical key to its values, and the `textproto_mime_header_` functions look keys up the same way Go's `MIMEHeader` methods do:

<!-- example: ../examples/net/textproto.c#header -->
```c
StringsReader *sr = strings_new_reader(
    a, BURROW_S("content-type: text/plain\r\nX-Tag: a\r\nx-tag: b\r\n\r\nbody"));
TextprotoReader *r =
    textproto_new_reader(a, bufio_new_reader(a, strings_reader_as_io_reader(sr)));
Error err;
TextprotoMIMEHeader h = textproto_reader_read_mime_header(r, a, &err);
printf("%.*s\n", P(textproto_mime_header_get(h, BURROW_S("Content-Type"))));
Slice tags = textproto_mime_header_values(h, BURROW_S("x-tag"));
printf("%d tags, first %.*s\n", (int)tags.len, P(((const Str *)tags.p)[0]));
printf("%.*s\n",
       P(textproto_canonical_mime_header_key(a, BURROW_S("x-forwarded-for"))));
```

That prints:

```
text/plain
2 tags, first a
X-Forwarded-For
```

A key that is one of the common headers comes back from a table with no allocation, and `textproto_canonical_mime_header_key` only allocates for a key it has to change that is not in that table.

`textproto_reader_read_response` reads a reply that may run over several lines, the way SMTP and FTP send them, and `textproto_reader_read_code_line` reads one line. When the code is not the one expected, the error is a `TextprotoError` with the code and message, and a reply that does not parse gives a `TextprotoProtocolError`. `errors_as` with `TYPE_TEXTPROTO_ERROR` or `TYPE_TEXTPROTO_PROTOCOL_ERROR` gets at them:

<!-- example: ../examples/net/textproto.c#codes -->
```c
StringsReader *sr = strings_new_reader(
    a, BURROW_S("250-mail.example.com\r\n250-SIZE 35882577\r\n250 HELP\r\n"
                "550 no such user\r\n"));
TextprotoReader *r =
    textproto_new_reader(a, bufio_new_reader(a, strings_reader_as_io_reader(sr)));
Error err;
Str msg;
Int code = textproto_reader_read_response(r, a, 250, &msg, &err);
printf("%d %.*s\n", (int)code, P(msg));
textproto_reader_read_code_line(r, a, 250, &msg, &err);
fmt_printf_v("%v\n", err);
```

That prints:

```
250 mail.example.com
SIZE 35882577
HELP
550 "no such user"
```

A `TextprotoWriter` writes lines with `textproto_writer_printf_line`, which takes the same format as `fmt_printf`, and `textproto_writer_dot_writer` returns an `IoWriteCloser` that escapes leading dots, turns `\n` into `\r\n` and writes the closing `.` line when it is closed:

<!-- example: ../examples/net/textproto.c#dot -->
```c
BytesBuffer out = BYTES_BUFFER(a);
TextprotoWriter *w =
    textproto_new_writer(a, bufio_new_writer(a, bytes_buffer_as_io_writer(&out)));
textproto_writer_printf_line_v(w, "DATA");
IoWriteCloser d = textproto_writer_dot_writer(w);
io_write_string(io_write_closer_as_io_writer(d), BURROW_S("line one\n.hidden\n"),
                NULL);
d.vt->closer.close(d.data);
fmt_printf_v("%q\n", bytes_buffer_string(&out, a));
```

That prints:

```
"DATA\r\nline one\r\n..hidden\r\n.\r\n"
```

A reader or writer holds one dot reader or writer, and asking for another, or reading or writing a line, finishes the one before it. Go does the same, except that there the old reader or writer is a separate value that goes stale; here it is the same one starting over. `TextprotoConn` puts a reader, a writer and a `TextprotoPipeline` over one connection. The `textproto_conn_` functions are the methods Go promotes from those three, and `textproto_conn_cmd` sends a command and returns its id for the pipeline. Go's `Dial` is not here yet, because it needs the `net` package; `textproto_new_conn` takes any `IoReadWriteCloser` in the meantime.

## Mail

`net/mail` follows Go's `net/mail`. `mail_read_message` reads the header of a message and leaves the body to be read from the message's `body`, and `mail_header_get`, `mail_header_date` and `mail_header_address_list` read the fields most programs want. The header is a `MailHeader`, which is the same `Map` as a `TextprotoMIMEHeader`, so the `textproto_mime_header_` functions work on it too. The header lives in an arena of the message's own, and `mail_message_free` gives it back with the reader:

<!-- example: ../examples/net/mail.c#message -->
```c
StringsReader *sr =
    strings_new_reader(a, BURROW_S("Date: Mon, 23 Jun 2015 11:40:36 -0400\n"
                                   "From: Gopher <from@example.com>\n"
                                   "To: Another Gopher <to@example.com>\n"
                                   "Subject: Gophers at Gophercon\n"
                                   "\n"
                                   "Message body\n"));
Error err;
MailMessage *m = mail_read_message(a, strings_reader_as_io_reader(sr), &err);
if (m == NULL) {
    fmt_printf_v("%v\n", err);
    return;
}
printf("Date: %.*s\n", P(mail_header_get(m->header, BURROW_S("Date"))));
printf("From: %.*s\n", P(mail_header_get(m->header, BURROW_S("From"))));
printf("Subject: %.*s\n", P(mail_header_get(m->header, BURROW_S("Subject"))));
Slice body = io_read_all(a, m->body, &err);
printf("%.*s", (int)body.len, (const char *)body.p);

Time t = mail_header_date(m->header, a, &err);
printf("%.*s\n", P(time_format(t, a, TIME_RFC3339)));
mail_message_free(m);
```

That prints:

```
Date: Mon, 23 Jun 2015 11:40:36 -0400
From: Gopher <from@example.com>
Subject: Gophers at Gophercon
Message body
2015-06-23T11:40:36-04:00
```

`mail_parse_address` reads one address and `mail_parse_address_list` reads a list, groups included. Each `MailAddress` is one allocation that holds the struct and both strings, so the input can go away while it lives, and a list is a `Slice` of `MailAddress *` that `mail_address_list_free` gives back in one call. `mail_address_string` puts an address back together the way RFC 5322 wants it, quoting the name, or encoding it as an RFC 2047 word when it is not plain ASCII:

<!-- example: ../examples/net/mail.c#addresses -->
```c
Error err;
MailAddress *e = mail_parse_address(a, BURROW_S("Alice <alice@example.com>"), &err);
if (e != NULL) {
    printf("%.*s %.*s\n", P(e->name), P(e->address));
    mail_address_free(a, e);
}

Slice list = mail_parse_address_list(
    a,
    BURROW_S(
        "Bob <bob@example.com>, eve@example.com, \"Gö, Pher\" <g@example.com>"),
    &err);
MailAddress **v = (MailAddress **)list.p;
for (Int i = 0; i < list.len; i++)
    printf("%.*s\n", P(mail_address_string(v[i], a)));
mail_address_list_free(a, list);

if (mail_parse_address(a, BURROW_S("John Doe"), &err) == NULL)
    fmt_printf_v("%v\n", err);
```

That prints:

```
Alice alice@example.com
"Bob" <bob@example.com>
<eve@example.com>
=?utf-8?b?R8O2LCBQaGVy?= <g@example.com>
mail: no angle-addr
```

Names written as RFC 2047 encoded words are decoded with a `MimeWordDecoder`. Without one, UTF-8, ISO-8859-1 and US-ASCII work and any other charset is an error, as in Go. To take more, put a decoder with a `charset_reader` in a `MailAddressParser` and call `mail_address_parser_parse` or `mail_address_parser_parse_list`. The parser follows the same parts of RFC 5322 Go does, and leaves out the same ones: obsolete forms such as routes are not read, an address cannot be folded across lines, and nothing is normalised.

## HTTP headers and content types

`net/http` is arriving in pieces, and the first piece is the part that needs no connection. An `HttpHeader` is a `TextprotoMIMEHeader` with Go's `Header` methods on it: `http_header_add` and `http_header_set` put the key into canonical form, `http_header_get` gives the first value, and `http_header_write` writes the header the way it goes on the wire, keys sorted, with any CR or LF in a value turned into a space so a value cannot start a header of its own:

<!-- example: ../examples/net/http.c#header -->
```c
HttpHeader h = http_header_make(a);
http_header_add(h, BURROW_S("accept-encoding"), BURROW_S("gzip"));
http_header_add(h, BURROW_S("Accept-Encoding"), BURROW_S("br"));
http_header_set(h, BURROW_S("content-type"), BURROW_S("text/html"));
http_header_set(h, BURROW_S("X-Note"), BURROW_S("one\r\nInjected: two"));
printf("%.*s\n", P(http_header_get(h, BURROW_S("CONTENT-TYPE"))));

BytesBuffer out = BYTES_BUFFER(a);
Error err = http_header_write(h, bytes_buffer_as_io_writer(&out));
if (BURROW_OK(err))
    fmt_printf_v("%q\n", bytes_buffer_string(&out, a));
```

That prints:

```
text/html
"Accept-Encoding: gzip\r\nAccept-Encoding: br\r\nContent-Type: text/html\r\nX-Note: one  Injected: two\r\n"
```

`http_header_write_subset` leaves out the keys in a `Map` from `Str` to `bool`, and `http_header_clone` copies a header with all its values in one block. Neither write allocates for a header of up to 64 keys.

`http_detect_content_type` is Go's `DetectContentType`, the WHATWG sniffing algorithm over the first 512 bytes, and it gives `application/octet-stream` when nothing matches. The status codes are `HTTP_STATUS_` constants and `http_status_text` gives Go's text for each:

<!-- example: ../examples/net/http.c#sniff -->
```c
Byte png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
char html[] = "  <!DOCTYPE html><title>hi</title>";
Str ct =
    http_detect_content_type(slice_from(png, sizeof png, sizeof png, TYPE_BYTE));
printf("%.*s\n", P(ct));
Int n = (Int)strlen(html);
ct = http_detect_content_type(slice_from(html, n, n, TYPE_BYTE));
printf("%.*s\n", P(ct));
printf("%d %.*s\n", HTTP_STATUS_TEAPOT, P(http_status_text(HTTP_STATUS_TEAPOT)));
```

That prints:

```
image/png
text/html; charset=utf-8
418 I'm a teapot
```

`http_parse_time` reads the three date formats HTTP has used, `HTTP_TIME_FORMAT` and the older RFC 850 and ANSI C ones, as Go's `ParseTime` does:

<!-- example: ../examples/net/http.c#time -->
```c
Error err;
Time t = http_parse_time(a, BURROW_S("Sunday, 06-Nov-94 08:49:37 GMT"), &err);
if (BURROW_OK(err))
    printf("%.*s\n", P(time_format(t, a, HTTP_TIME_FORMAT)));
http_parse_time(a, BURROW_S("yesterday"), &err);
printf("%s\n", BURROW_FAILED(err) ? "not a date" : "a date");
```

That prints:

```
Sun, 06 Nov 1994 08:49:37 GMT
not a date
```

## Cookies

`HttpCookie` is Go's `Cookie`. `http_parse_set_cookie` reads the value of a `Set-Cookie` header from a response, and `http_parse_cookie` reads a `Cookie` header from a request, which can hold many cookies. The strings in a parsed cookie point into the line, and an attribute the parser doesn't know goes in `unparsed` as it was written:

<!-- example: ../examples/net/http.c#cookie -->
```c
Error err;
Str line =
    BURROW_S("session=38afes7a8; Path=/; Max-Age=3600; HttpOnly; Flavour=mint");
HttpCookie c = http_parse_set_cookie(a, line, &err);
if (BURROW_OK(err)) {
    printf("%.*s=%.*s, path %.*s, max-age %d\n", P(c.name), P(c.value), P(c.path),
           (int)c.max_age);
    printf("unparsed: %.*s\n", P(BURROW_AT(Str, c.unparsed, 0)));
}

Slice sent = http_parse_cookie(a, BURROW_S("lang=en; theme=\"dark\""), &err);
for (Int i = 0; i < sent.len; i++) {
    HttpCookie k = BURROW_AT(HttpCookie, sent, i);
    printf("%.*s is %.*s%s\n", P(k.name), P(k.value), k.quoted ? ", quoted" : "");
}

HttpCookie out = {
    .name = BURROW_S_INIT("cart"),
    .value = BURROW_S_INIT("3 items"),
    .path = BURROW_S_INIT("/shop"),
    .expires = time_date(2030, TIME_MARCH, 1, 12, 0, 0, 0, time_utc_loc),
    .secure = true,
    .same_site = HTTP_SAME_SITE_STRICT_MODE,
};
printf("%.*s\n", P(http_cookie_string(a, &out)));
```

That prints:

```
session=38afes7a8, path /, max-age 3600
unparsed: Flavour=mint
lang is en
theme is dark, quoted
cart="3 items"; Path=/shop; Expires=Fri, 01 Mar 2030 12:00:00 GMT; Secure; SameSite=Strict
```

`http_cookie_string` goes the other way, and drops the bytes a value or path can't hold, quoting a value with a space or comma in it as Go does. Each of those is reported on the standard logger from `burrow/log.h`, as Go reports it with `log.Printf`. `http_cookie_valid` says whether a cookie could be sent as it is. A request with more than 3000 cookies gets an error, and `GODEBUG=httpcookiemaxnum=N` moves that limit, with 0 taking it away.

## Reading requests and responses

`http_read_request` reads one request off a `BufioReader`, the way a server does, and `http_read_response` reads one response, the way a client does. Each hands back a message with its own arena, and `http_request_free` or `http_response_free` gives all of it back at once, the body included. The body reads only its own bytes, whether they come with a `Content-Length`, in chunks, or up to the end of the connection, so the next message can be read from the same reader once it is done:

<!-- example: ../examples/net/http.c#wire -->
```c
StringsReader sr;
strings_reader_reset(&sr, BURROW_S("POST /upload?name=notes HTTP/1.1\r\n"
                                   "Host: example.com\r\n"
                                   "Authorization: Basic YWxpY2U6czNjcmV0\r\n"
                                   "Content-Length: 11\r\n"
                                   "\r\n"
                                   "hello world"));
BufioReader *br = bufio_new_reader(a, strings_reader_as_io_reader(&sr));
Error err;
HttpRequest *req = http_read_request(a, br, &err);
if (BURROW_OK(err)) {
    printf("%.*s %.*s for %.*s, %lld bytes\n", P(req->method), P(req->url->path),
           P(req->host), (long long)req->content_length);
    Str user;
    Str pass;
    if (http_request_basic_auth(req, a, &user, &pass))
        printf("from %.*s\n", P(user));
    Slice body = io_read_all(a, io_read_closer_as_io_reader(req->body), &err);
    printf("body: %.*s\n", (int)body.len, (const char *)body.p);
    http_request_free(req);
}
bufio_reader_free(br);

strings_reader_reset(&sr, BURROW_S("HTTP/1.1 404 Not Found\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Transfer-Encoding: chunked\r\n"
                                   "\r\n"
                                   "4\r\nnone\r\n5\r\n here\r\n0\r\n\r\n"));
br = bufio_new_reader(a, strings_reader_as_io_reader(&sr));
HttpResponse *resp = http_read_response(a, br, NULL, &err);
if (BURROW_OK(err)) {
    printf("%d, %.*s\n", (int)resp->status_code, P(resp->status));
    Slice body = io_read_all(a, io_read_closer_as_io_reader(resp->body), &err);
    printf("body: %.*s\n", (int)body.len, (const char *)body.p);
    http_response_free(resp);
}
bufio_reader_free(br);

strings_reader_reset(&sr, BURROW_S("HTTP/1.1 OK\r\n\r\n"));
br = bufio_new_reader(a, strings_reader_as_io_reader(&sr));
http_read_response(a, br, NULL, &err);
printf("%.*s\n", P(error_text(err)));
bufio_reader_free(br);
```

That prints:

```
POST /upload for example.com, 11 bytes
from alice
body: hello world
404, 404 Not Found
body: none here
malformed HTTP status code "OK"
```

As in Go, a request's `Host` field is taken out of its header and kept in `host`. The response is given the request it answers, or NULL, because a response to `HEAD` has no body whatever its header says. `Transfer-Encoding` other than a single `chunked` is refused, and so are two different `Content-Length` values, since both are ways to smuggle one request inside another.

## Routing requests

`HttpServeMux` is Go's `ServeMux`, with the patterns Go 1.22 brought in. A pattern can name a method and a host, and a wildcard such as `{id}` matches one segment of the path, which the handler gets back with `http_request_path_value`. A pattern ending in a slash matches everything under it, and `{$}` at the end matches only the slash. When two patterns match a request the more specific one wins, and two patterns that match the same requests with neither more specific panic when the second is registered, with the file and line of the first in the message. `GODEBUG=httpmuxgo121=1` brings back the Go 1.21 mux, which has none of this.

A handler is an `HttpHandler`, a vtable and a pointer like any other interface, and `HttpHandlerFunc` makes one from a function and its environment. `http_serve_mux_serve_http` finds the handler for a request and calls it. A path that isn't clean is redirected to the clean one, a path that is only registered with a slash on the end is redirected there, and a method no pattern allows gets a 405 with an `Allow` header listing the ones that are. There is no server yet, so here a small `ResponseWriter` from the example file prints what each request gets:

<!-- example: ../examples/net/http.c#mux -->
```c
HttpServeMux *mux = http_new_serve_mux(a);
http_serve_mux_handle_func(mux, BURROW_S("GET /notes/{id}"),
                           BURROW_FN(HttpHandlerFunc, show_note, NULL));
http_serve_mux_handle_func(mux, BURROW_S("/files/"),
                           BURROW_FN(HttpHandlerFunc, show_file, NULL));

const char *reqs[][2] = {
    {"GET", "/notes/42"}, {"DELETE", "/notes/42"},      {"GET", "/files/a.txt"},
    {"GET", "/files"},    {"GET", "/files/x/../b.txt"}, {"GET", "/other"},
};
for (size_t i = 0; i < sizeof reqs / sizeof reqs[0]; i++) {
    Error err;
    HttpRequest *r =
        http_new_request(a, str_from_cstr(reqs[i][0]), str_from_cstr(reqs[i][1]),
                         (IoReader){0}, &err);
    if (r == NULL)
        continue;
    printf("%s %s: ", reqs[i][0], reqs[i][1]);
    Printer out = {http_header_make(a), 0};
    http_serve_mux_serve_http(mux, (HttpResponseWriter){&printer_vt, &out}, r);
    http_request_free(r);
}
http_serve_mux_free(mux);
```

That prints:

```
GET /notes/42: 200
    note 42
DELETE /notes/42: 405, allow GET, HEAD
GET /files/a.txt: 200
    file /files/a.txt
GET /files: 307 to /files/
GET /files/x/../b.txt: 307 to /files/b.txt
GET /other: 404
```

`http_serve_mux_handler` answers the same question without calling anything, and gives back the pattern that matched. `http_strip_prefix`, `http_redirect_handler` and `http_not_found_handler` are the small handlers Go has, and `http_handle` registers on `http_default_serve_mux` as `http.Handle` does.

## Testing handlers

`net/http/httptest` tests a handler without a network. `httptest_new_request` makes a request the way a server hands one to a handler: the target is read as the target of a request line, the request is HTTP/1.1, and its host is example.com unless the target names one. It panics on arguments that don't make a request line, since in a test that is a bug in the test. An `HttptestResponseRecorder` is a response writer that keeps the status, the header and the body, and `httptest_response_recorder_result` turns what the handler did into the `HttpResponse` a client would have read, with the header as it was when the body began:

<!-- example: ../examples/net/http.c#httptest -->
```c
HttpHandlerFunc h = BURROW_FN(HttpHandlerFunc, hello, NULL);
const char *methods[] = {"GET", "POST"};
for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
    HttpRequest *r = httptest_new_request(a, str_from_cstr(methods[i]),
                                          BURROW_S("/greet"), (IoReader){0});
    HttptestResponseRecorder *rec = httptest_new_recorder(a);
    http_handler_func_serve_http(
        h, httptest_response_recorder_as_response_writer(rec), r);

    HttpResponse *res = httptest_response_recorder_result(rec);
    Error err;
    Slice body = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
    fmt_printf_v("%s %s\n", r->method, res->status);
    fmt_printf_v("  Content-Type: %s\n",
                 http_header_get(res->header, BURROW_S("Content-Type")));
    fmt_printf_v("  Cache-Control: %s\n",
                 http_header_get(res->header, BURROW_S("Cache-Control")));
    Str text = {(const Byte *)body.p, body.len};
    fmt_printf_v("  body: %q\n", text);
    httptest_response_recorder_free(rec);
    http_request_free(r);
}
```

That prints:

```
GET 200 OK
  Content-Type: text/html; charset=utf-8
  Cache-Control: no-store
  body: "<p>hello from /greet</p>"
POST 405 Method Not Allowed
  Content-Type: text/plain; charset=utf-8
  Cache-Control: no-store
  body: "only GET here\n"
```

The recorder sniffs a Content-Type from the first write when the handler sets none, as a server does, and a write after a 204 or 304 status keeps the bytes but returns `http_err_body_not_allowed`. Go's `httptest.Server` needs the HTTP server and comes with it.

## Serving files

`http_file_server` serves the files of an `HttpFileSystem`, and `http_file_server_fs` serves an `Fs` from io/fs, such as `os_dir_fs` or the in-memory `FstestMapFS` here. A request for a directory gets its index.html or a listing. A request for index.html itself is redirected to the directory, and so is a directory asked for without its slash. The handler answers conditional and range requests through `http_serve_content`, which works on any `IoReadSeeker` when the content isn't a file:

<!-- example: ../examples/net/http.c#files -->
```c
FstestMapFS site = fstest_map_fs_make(a);
FstestMapFile page = {
    .data = slice_from_str(a, BURROW_S("<h1>burrow</h1>\n")),
    .mod_time = time_date(2026, TIME_JANUARY, 2, 15, 4, 5, 0, time_utc_loc),
};
FstestMapFile notes = {.data = slice_from_str(a, BURROW_S("0123456789\n"))};
fstest_map_fs_set(site, BURROW_S("index.html"), &page);
fstest_map_fs_set(site, BURROW_S("notes.txt"), &notes);
HttpHandler h = http_file_server_fs(a, fstest_map_fs_as_fs(site));

const struct {
    const char *target, *key, *value;
} reqs[] = {
    {"/", NULL, NULL},
    {"/", "If-Modified-Since", "Fri, 02 Jan 2026 15:04:05 GMT"},
    {"/notes.txt", "Range", "bytes=2-5"},
    {"/index.html", NULL, NULL},
    {"/missing.txt", NULL, NULL},
};
const char *show[] = {"Content-Type", "Content-Range", "Last-Modified", "Location"};
for (size_t i = 0; i < sizeof reqs / sizeof reqs[0]; i++) {
    HttpRequest *r = httptest_new_request(
        a, BURROW_S("GET"), str_from_cstr(reqs[i].target), (IoReader){0});
    fmt_printf_v("GET %s", r->url->path);
    if (reqs[i].key != NULL) {
        Str key = str_from_cstr(reqs[i].key), value = str_from_cstr(reqs[i].value);
        http_header_set(r->header, key, value);
        fmt_printf_v(" with %s: %s", key, value);
    }
    HttptestResponseRecorder *rec = httptest_new_recorder(a);
    http_handler_serve_http(h, httptest_response_recorder_as_response_writer(rec),
                            r);

    HttpResponse *res = httptest_response_recorder_result(rec);
    fmt_printf_v("\n  %s\n", res->status);
    for (size_t j = 0; j < sizeof show / sizeof show[0]; j++) {
        Str v = http_header_get(res->header, str_from_cstr(show[j]));
        if (v.len > 0)
            fmt_printf_v("  %s: %s\n", str_from_cstr(show[j]), v);
    }
    Slice body = bytes_buffer_bytes(rec->body);
    if (body.len > 0)
        fmt_printf_v("  body: %q\n", str_from_bytes(body.p, body.len));
    httptest_response_recorder_free(rec);
    http_request_free(r);
}
```

That prints:

```
GET /
  200 OK
  Content-Type: text/html; charset=utf-8
  Last-Modified: Fri, 02 Jan 2026 15:04:05 GMT
  body: "<h1>burrow</h1>\n"
GET / with If-Modified-Since: Fri, 02 Jan 2026 15:04:05 GMT
  304 Not Modified
  Last-Modified: Fri, 02 Jan 2026 15:04:05 GMT
GET /notes.txt with Range: bytes=2-5
  206 Partial Content
  Content-Type: text/plain; charset=utf-8
  Content-Range: bytes 2-5/11
  body: "2345"
GET /index.html
  301 Moved Permanently
  Location: ./
GET /missing.txt
  404 Not Found
  Content-Type: text/plain; charset=utf-8
  body: "404 page not found\n"
```

The Content-Type comes from the file's extension, and from sniffing the first 512 bytes when the extension is unknown. A file with a zero modification time gets no Last-Modified, which is why notes.txt has none. `http_serve_file` and `http_serve_file_fs` serve one named file, and refuse a request whose path has a `..` element in it.

## Cookie jars

`net/http/cookiejar` keeps the cookies a client is sent and picks the ones each request should carry, by the rules of RFC 6265. A cookie with no Domain attribute only goes back to the host that set it, and one with a Domain goes to that domain and every name under it, as long as the host that set it is in that domain. A Secure cookie only goes over https, or to localhost. Cookies with longer paths come first, and among those the ones set earlier come first. A Max-Age of 0, or an Expires in the past, deletes the cookie it names:

<!-- example: ../examples/net/http.c#cookiejar -->
```c
Error err;
CookiejarJar *jar = cookiejar_new(a, NULL, &err);
Url *from = url_parse(a, BURROW_S("http://www.example.com/shop/"), &err);
const char *lines[] = {
    "session=1; Path=/",       "lang=en; Domain=example.com",
    "cart=3; Max-Age=3600",    "track=x; Domain=other.com",
    "admin=1; Path=/; Secure",
};
Slice set = slice_make(a, TYPE_HTTP_COOKIE, 0, 5);
for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) {
    HttpCookie c = http_parse_set_cookie(a, str_from_cstr(lines[i]), &err);
    set = slice_append(a, set, &c, 1);
}
cookiejar_jar_set_cookies(jar, from, set);

show_cookies(jar, a, "http://www.example.com/shop/basket");
show_cookies(jar, a, "https://www.example.com/");
show_cookies(jar, a, "https://api.example.com/");
show_cookies(jar, a, "http://other.com/");

HttpCookie gone =
    http_parse_set_cookie(a, BURROW_S("session=; Path=/; Max-Age=0"), &err);
cookiejar_jar_set_cookies(jar, from, slice_from(&gone, 1, 1, TYPE_HTTP_COOKIE));
show_cookies(jar, a, "https://www.example.com/shop/basket");
cookiejar_jar_free(jar);
```

That prints:

```
http://www.example.com/shop/basket: lang=en cart=3 session=1
https://www.example.com/: session=1 admin=1
https://api.example.com/:
http://other.com/:
https://www.example.com/shop/basket: lang=en cart=3 admin=1
```

`cookiejar_new` takes options with a public suffix list, which is what stops a server for foo.co.uk from setting a cookie for every site under co.uk. Without one, as here, the jar can't tell co.uk from example.com, so don't leave it out in a client that talks to sites you don't trust. Go's list is in golang.org/x/net/publicsuffix, which burrow doesn't have yet, and any `CookiejarPublicSuffixList` you write works. The jar copies what it keeps into the allocator you gave `cookiejar_new`, so the cookies you hand it can go away as soon as the call returns, and `cookiejar_jar_cookies` gives copies in the allocator you pass. `cookiejar_jar_as_cookie_jar` gives the jar as an `HttpCookieJar`, the interface Go's client takes.

## Running a server

`HttpServer` is Go's `http.Server`. Fill in the fields you want, leave the rest zero, and hand it a listener with `http_server_serve`, or let `http_server_listen_and_serve` listen on its `addr`. Each connection is served on a goroutine of its own, so the handler has to be safe to call from several at once. Keep-alive, `Expect: 100-continue`, chunked responses when the handler sets no length, and the 400s, 431s and 505s for requests that are wrong all work the way they do in Go. The example listens on a port the system picks, sends two requests the long way over TCP, and shuts down:

<!-- example: ../examples/net/server.c#server -->
```c
Byte loopback[4] = {127, 0, 0, 1};
NetTCPAddr laddr = {slice_from(loopback, 4, 4, TYPE_BYTE), 0, BURROW_STR_EMPTY};
Error err;
NetTCPListener *l = net_listen_tcp(heap_allocator(), BURROW_S("tcp"), &laddr, &err);
if (l == NULL)
    return;
Int port = ((const NetTCPAddr *)net_tcp_listener_addr(l).data)->port;

HttpServeMux *mux = http_new_serve_mux(heap_allocator());
http_serve_mux_handle_func(mux, BURROW_S("GET /hello/{name}"),
                           BURROW_FN(HttpHandlerFunc, greet, NULL));
HttpServer srv = {.handler = http_serve_mux_as_handler(mux),
                  .read_header_timeout = 5 * TIME_SECOND};
Serving s = {&srv, net_tcp_listener_as_listener(l), BURROW_NO_ERROR};
SyncWaitGroup wg = {0};
sync_wait_group_go(&wg, BURROW_FN(Func, serve, &s));

ask(a, port,
    "GET /hello/gopher HTTP/1.1\r\nHost: example\r\nConnection: close\r\n\r\n");
ask(a, port,
    "DELETE /hello/gopher HTTP/1.1\r\nHost: example\r\nConnection: close\r\n\r\n");

/* Shutdown stops taking connections and waits for the open ones to go
 * idle, for five seconds at most. */
ContextCancelFunc cancel;
Context ctx = context_with_timeout(heap_allocator(), context_background(),
                                   5 * TIME_SECOND, &cancel);
err = http_server_shutdown(&srv, ctx);
BURROW_CALLF0(cancel);
context_release(ctx);
sync_wait_group_wait(&wg);
fmt_printf_v("shutdown: %v\nserve: %v\n", err, s.err);
http_server_free(&srv);
http_serve_mux_free(mux);
net_tcp_listener_free(l);
```

That prints:

```
HTTP/1.1 200 OK
Content-Length: 14
Content-Type: text/plain; charset=utf-8
Connection: close

hello, gopher
HTTP/1.1 405 Method Not Allowed
Allow: GET, HEAD
Content-Type: text/plain; charset=utf-8
X-Content-Type-Options: nosniff
Content-Length: 19
Connection: close

Method Not Allowed
shutdown: <nil>
serve: http: Server closed
```

`http_server_shutdown` closes the listeners, so `http_server_serve` returns `http_err_server_closed` at once, and then waits for each connection to finish its request and go idle, or for the context to be done. `http_server_close` doesn't wait: it closes every connection there and then. A handler that panics gets its connection closed and the panic logged with a stack trace to `error_log`, or to the standard logger when that is NULL, and the server goes on serving the others. A handler that wants to stop without a log line panics with `http_err_abort_handler`.

A server can't be copied once it has started, and `http_server_free` waits for the goroutines of all its connections to end, so call it after Serve has returned. A connection from a `NetTCPListener` or `NetUnixListener` is freed by the server when it is done, and one from any other listener is closed but stays yours to free. `http_timeout_handler`, `http_max_bytes_handler` and `http_allow_query_semicolons` wrap a handler the way Go's do, and `HttpResponseController` reaches the flush, hijack and deadline methods of the writer a handler gets. Only HTTP/1 is here so far, without TLS.

## Making requests

An `HttpClient` sends requests and follows redirects. `http_client_get`, `http_client_head`, `http_client_post` and `http_client_post_form` cover the common cases, and `http_client_do` sends a request you made with `http_new_request`. A zero client works, with `http_default_transport` underneath, which keeps connections open between requests and shares them. `http_get` and the rest use `http_default_client`. An `httptest_new_server` runs a handler on a loopback port, so a test can talk to it with a real client, and its `httptest_server_client` already knows the way:

<!-- example: ../examples/net/client.c#client -->
```c
HttptestServer *ts =
    httptest_new_server(heap_allocator(), http_serve_mux_as_handler(mux));
HttpClient *c = httptest_server_client(ts);
Error err;

/* Get follows the redirect. */
Str url = fmt_sprintf_v(a, "%s/old", ts->url);
show(a, http_client_get(c, url, &err), err);

/* PostForm encodes the values as the body. */
UrlValues form = url_values_make(a);
url_values_set(form, BURROW_S("name"), BURROW_S("gopher"));
url_values_add(form, BURROW_S("name"), BURROW_S("burrow"));
show(a, http_client_post_form(c, fmt_sprintf_v(a, "%s/echo", ts->url), form, &err),
     err);

/* check_redirect can stop at the redirect and hand it back. */
HttpClient stopping = *c;
stopping.check_redirect = BURROW_FN(HttpCheckRedirectFunc, stop_here, NULL);
HttpResponse *res = http_client_get(&stopping, url, &err);
if (res != NULL)
    fmt_printf_v("Location: %s\n",
                 http_header_get(res->header, BURROW_S("Location")));
show(a, res, err);

/* timeout covers the whole exchange, body and all. */
HttpClient hurried = *c;
hurried.timeout = 100 * TIME_MILLISECOND;
res = http_client_get(&hurried, fmt_sprintf_v(a, "%s/slow", ts->url), &err);
fmt_printf_v("deadline exceeded: %t, timeout: %t\n",
             errors_is(err, context_deadline_exceeded),
             (bool)(res == NULL && net_error_timeout(err)));
http_response_free(res);
httptest_server_free(ts);
```

That prints:

```
200 OK from /new: you found the new page
200 OK from /echo: application/x-www-form-urlencoded name=gopher&name=burrow
Location: /new
302 Found from /old: <a href="/new">Found</a>.

deadline exceeded: true, timeout: true
```

The client follows up to ten redirects, and `check_redirect` decides otherwise: an error from it ends the request, and `http_err_use_last_response` hands back the redirect itself, body unread. A 301, 302 or 303 turns the request into a GET with no body, while a 307 or 308 sends the same method and body again, which takes a `get_body` when the body isn't a `BytesBuffer`, `BytesReader` or `StringsReader`, the three `http_new_request` knows how to rewind. Headers go along to the same host and its subdomains, but `Authorization`, `Cookie` and the other credential headers stop at a different domain. With a `jar`, cookies from each response go into the jar and the jar's cookies go out with each request.

`timeout` covers the whole exchange, the redirects and the reading of the body included. When it runs out the error is a `UrlError` that is a timeout to `net_error_timeout`, and `errors_is` finds `context_deadline_exceeded` in it. A request's context can cancel it too. Every error from the client is a `UrlError` with the method and the URL, with any password in the URL shown as `***`. A status like 404 is not an error. The response it comes with has to be freed with `http_response_free`, and so does the one that comes back with an error from `check_redirect`.

## Tracing requests

`net/http/httptrace` shows what the client does on the way to a response. An `HttptraceClientTrace` is a set of hooks, any of which can be left out, for getting a connection, the DNS lookup and the dial, writing the request and reading the response. `httptrace_with_client_trace` puts it in a context, and a request sent with that context calls the hooks:

<!-- example: ../examples/net/httptrace.c#hooks -->
```c
static void get_conn(void *env, Str host_port) {
    (void)env;
    (void)host_port;
    fmt_printf_v("get conn\n");
}

static void got_conn(void *env, HttptraceGotConnInfo info) {
    (void)env;
    fmt_printf_v("got conn, reused: %t\n", info.reused);
}

static void wrote_headers(void *env) {
    (void)env;
    fmt_printf_v("wrote headers\n");
}

static void first_byte(void *env) {
    (void)env;
    fmt_printf_v("first response byte\n");
}

static void put_idle_conn(void *env, Error err) {
    (void)env;
    fmt_printf_v("put idle conn: %v\n", err);
}
```

<!-- example: ../examples/net/httptrace.c#trace -->
```c
HttptraceClientTrace trace = {
    .get_conn = BURROW_FN(HttptraceGetConnFunc, get_conn, NULL),
    .got_conn = BURROW_FN(HttptraceGotConnFunc, got_conn, NULL),
    .wrote_headers = BURROW_FN(Func, wrote_headers, NULL),
    .got_first_response_byte = BURROW_FN(Func, first_byte, NULL),
    .put_idle_conn = BURROW_FN(HttptracePutIdleConnFunc, put_idle_conn, NULL),
};
Context ctx = httptrace_with_client_trace(a, context_background(), &trace);

/* The second request gets the connection the first one left idle. */
for (int i = 0; i < 2; i++) {
    Error err;
    IoReader none = {NULL, NULL};
    HttpRequest *req =
        http_new_request_with_context(a, ctx, BURROW_S("GET"), ts->url, none, &err);
    HttpResponse *res = http_client_do(c, req, &err);
    if (res == NULL) {
        fmt_printf_v("error: %v\n", err);
        break;
    }
    Slice b = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
    Str status = str_clone(a, res->status);
    http_response_free(res);
    fmt_printf_v("%s: %s", status, str_from_bytes(b.p, b.len));
}
context_release(ctx);
```

That prints:

```
get conn
got conn, reused: false
wrote headers
first response byte
put idle conn: <nil>
200 OK: hello
get conn
got conn, reused: true
wrote headers
first response byte
put idle conn: <nil>
200 OK: hello
```

The hooks run on the transport's goroutines, so one that shares state with the caller needs a lock, and the trace has to live until the last response sent with it is freed. A trace put in a context that already has one runs its own hooks and then the older one's, as in Go. Unlike Go, it does that by keeping a pointer to the older trace rather than rewriting its own fields, so code that wants to run the hooks itself calls `httptrace_client_trace_got_conn` and the rest. The hooks for the dial stop once the request has its connection, while in Go a dial that lost the race to an idle connection keeps calling them. There are no TLS hooks yet.

## Dumping requests and responses

`net/http/httputil` can show a message the way it goes over the wire, which helps when debugging a client or a server. `httputil_dump_request_out` shows a request as the transport would send it, with the `User-Agent`, `Accept-Encoding` and other fields the transport adds, by sending it through a transport to a connection that only records it. `httputil_dump_request` shows a request as a server got it, and `httputil_dump_response` shows a response:

<!-- example: ../examples/net/httputil.c#dump -->
```c
/* A request as the transport would send it, with the fields it adds. */
StringsReader body;
strings_reader_reset(&body, BURROW_S("name=gopher"));
Error err;
HttpRequest *req = http_new_request(a, BURROW_S("POST"),
                                    BURROW_S("http://example.com/signup?ref=docs"),
                                    strings_reader_as_io_reader(&body), &err);
if (req == NULL)
    return;
http_header_set(req->header, BURROW_S("Content-Type"),
                BURROW_S("application/x-www-form-urlencoded"));
Slice out = httputil_dump_request_out(a, req, true, &err);
if (BURROW_OK(err))
    fmt_printf_v("%q\n", str_from_bytes(out.p, out.len));
http_request_free(req);

/* A response, with its body read into the dump and put back after. */
StringsReader sr;
strings_reader_reset(&sr, BURROW_S("HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: 5\r\n"
                                   "\r\n"
                                   "hello"));
BufioReader *br = bufio_new_reader(a, strings_reader_as_io_reader(&sr));
HttpResponse *res = http_read_response(a, br, NULL, &err);
if (res != NULL) {
    out = httputil_dump_response(a, res, false, &err);
    fmt_printf_v("%q\n", str_from_bytes(out.p, out.len));
    out = httputil_dump_response(a, res, true, &err);
    fmt_printf_v("%q\n", str_from_bytes(out.p, out.len));
    Slice b = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
    fmt_printf_v("body after the dump: %s\n", str_from_bytes(b.p, b.len));
    http_response_free(res);
}
bufio_reader_free(br);
```

With `body` true, a dump reads the whole body into memory, and the request or response gets a reader over those same bytes, so it can still be used after. With `body` false the body is left out, but the header keeps the length the body would have had. `httputil_new_chunked_reader` and `httputil_new_chunked_writer` are the chunked encoding the client and server use, for code that speaks HTTP/1 itself.

## Reverse proxies

`net/http/httputil` has a reverse proxy, a handler that sends each request it gets on to another server and copies the response back. An `HttputilReverseProxy` needs `rewrite` to say where a request goes. It gets an `HttputilProxyRequest` with `in`, the request the proxy got, and `out`, the copy it will send. `httputil_proxy_request_set_url` points `out` at a target, joining the paths, and `httputil_proxy_request_set_x_forwarded` adds the `X-Forwarded-For`, `X-Forwarded-Host` and `X-Forwarded-Proto` fields. `modify_response`, when set, can change the response before it is copied:

<!-- example: ../examples/net/reverseproxy.c#hooks -->
```c
static void rewrite(void *env, HttputilProxyRequest *r) {
    const Url *target = env;
    httputil_proxy_request_set_url(r, target);
    httputil_proxy_request_set_x_forwarded(r);
    http_header_set(r->out->header, BURROW_S("X-Api-Key"), BURROW_S("secret"));
}

static Error modify_response(void *env, HttpResponse *res) {
    (void)env;
    http_header_set(res->header, BURROW_S("X-Proxied"), BURROW_S("yes"));
    return BURROW_NO_ERROR;
}
```

<!-- example: ../examples/net/reverseproxy.c#proxy -->
```c
Url *target = url_parse(a, fmt_sprintf_v(a, "%s/api", back->url), &err);
HttputilReverseProxy proxy = {
    .rewrite = BURROW_FN(HttputilRewriteFunc, rewrite, target),
    .modify_response = BURROW_FN(HttputilModifyResponseFunc, modify_response, NULL),
};
HttptestServer *front =
    httptest_new_server(heap_allocator(), httputil_reverse_proxy_as_handler(&proxy));

HttpResponse *res = http_client_get(httptest_server_client(front),
                                    fmt_sprintf_v(a, "%s/users", front->url), &err);
if (res != NULL) {
    Slice b = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
    fmt_printf_v("%s, X-Proxied %s\n", res->status,
                 http_header_get(res->header, BURROW_S("X-Proxied")));
    fmt_printf_v("%s", str_from_bytes(b.p, b.len));
    http_response_free(res);
}
```

That prints:

```
200 OK, X-Proxied yes
path /api/users, X-Forwarded-Proto http
```

Before `rewrite` runs, the proxy takes the hop-by-hop fields off `out`, the ones `Connection` names and the standard ones such as `Keep-Alive` and `Proxy-Authorization`, along with `Forwarded`, the `X-Forwarded` fields and any query parameters that don't parse. So a backend only sees forwarding fields the rewrite put there. `httputil_new_single_host_reverse_proxy` makes a proxy the older way, with a `director` that changes the request in place. That one leaves the `Host` field as the client sent it, and in that mode the proxy adds the client's address to `X-Forwarded-For` itself.

When the backend can't be reached, or `modify_response` returns an error, `error_handler` gets the error, and without one the proxy logs it to `error_log` and answers 502 Bad Gateway. A 101 Switching Protocols from the backend, to an upgrade the client asked for, takes over the client's connection and copies bytes both ways, which is how WebSockets go through. `flush_interval` says how often the body is flushed to the client while it is copied, and a response of unknown length or of type `text/event-stream` is flushed after every write.

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

`ReadMsgUDP` and `WriteMsgUDP` are still to come, and `ListenMulticastUDP` is under [Interfaces](#interfaces), since it takes one. Windows and wasip1 are where TCP is: a dial or a listen on Windows fails with `ENOSYS` for now, and wasip1 has no sockets.

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

The datagram functions are the UDP ones with a `NetUnixAddr` in place of a `NetUDPAddr`: `net_unix_conn_read_from_unix` makes the sender's address in the allocator it is given, and `net_unix_conn_write_to_unix` sends to a name. `ReadMsgUnix`, `WriteMsgUnix` and the `File` methods, which pass descriptors, are still to come, and Windows is where TCP is for now.

## Raw IP

`net_listen_ip` and `net_dial_ip` are Go's `ListenIP` and `DialIP`, which make raw IP sockets. The network is "ip", "ip4" or "ip6" with the protocol after a colon, by number or by the name /etc/protocols gives it, so "ip4:icmp" and "ip4:1" are the same thing. A `NetIPAddr` is an address and a zone with no port. The system only hands out raw sockets to root, or to a program with CAP_NET_RAW on Linux, and anyone else gets an error, which is EPERM on Linux. This one sends an ICMP echo request to the loopback address and waits for the reply:

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

On an IPv4 socket, `net_ip_conn_read_from_ip` and the `read_from` of the `NetPacketConn` take the IPv4 header off the front of each packet, as Go's `ReadFromIP` does, while `net_ip_conn_read` gives the packet as the system hands it over, header and all. `ReadMsgIP` and `WriteMsgIP` are still to come, and Windows is where TCP is for now.

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

# Networking

`burrow/net/netip.h` is Go's `net/netip`, which holds IP addresses, address and port pairs, and CIDR prefixes as small values. `burrow/net/url.h` is Go's `net/url`, which parses, builds and resolves URLs and query strings. `burrow/net.h` has the address types from Go's `net` package itself, `IP`, `IPMask` and `IPNet`, with `SplitHostPort` and `JoinHostPort`. The rest of `net` (sockets, the resolver, `net/http` and friends) sits on top of the runtime's network poller and will land in this guide as it is ported.

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
PENDING
```

`http_serve_mux_handler` answers the same question without calling anything, and gives back the pattern that matched. `http_strip_prefix`, `http_redirect_handler` and `http_not_found_handler` are the small handlers Go has, and `http_handle` registers on `http_default_serve_mux` as `http.Handle` does.

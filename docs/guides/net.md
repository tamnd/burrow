# Networking

`burrow/net/netip.h` is Go's `net/netip`, which holds IP addresses, address and port pairs, and CIDR prefixes as small values. `burrow/net/url.h` is Go's `net/url`, which parses, builds and resolves URLs and query strings. The rest of `net` (sockets, the resolver, `net/http` and friends) sits on top of the runtime's network poller and will land in this guide as it is ported.

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

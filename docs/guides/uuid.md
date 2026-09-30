# UUIDs

`burrow/uuid.h` is Go 1.27's `uuid` package: RFC 9562 identifiers, made new or read from text. A `Uuid` is sixteen bytes in a struct, so it is passed, returned and assigned by value like any small struct.

## Making one

`uuid_new` is the one to use when nothing says otherwise. Today it gives a version 4 UUID, 122 random bits from the operating system's generator, the same source Go's `crypto/rand` reads:

<!-- example: ../examples/uuid/uuid.c#new -->
```c
Uuid id = uuid_new();
Str s = uuid_string(id, a);
printf("%d %c\n", (int)s.len, s.p[14]); /* 36 4 */
```

`uuid_new_v7` puts the Unix time in milliseconds at the front, followed by a fraction of a millisecond and then random bits. Each one sorts after the one before, even when two are made in the same instant, so they make good database keys. The only exception is the system clock moving backwards:

<!-- example: ../examples/uuid/uuid.c#v7 -->
```c
Uuid first = uuid_new_v7();
Uuid second = uuid_new_v7();
printf("%d\n", (int)uuid_cmp(first, second)); /* -1 */
```

Inside a synctest bubble `uuid_new_v7` reads the bubble's clock, the way Go's reads the bubble's `time.Now`, so a test that sleeps an hour in a bubble gets UUIDs an hour apart.

## Text

`uuid_string` writes the 36 character lowercase form. `uuid_parse_str` reads that form, the same with braces around it, with `urn:uuid:` in front, or as 32 hex digits with no dashes, in either case. Anything else is an error that reads `invalid uuid`:

<!-- example: ../examples/uuid/uuid.c#parse -->
```c
Error err;
Uuid u =
    uuid_parse_str(BURROW_S("{F81D4FAE-7DEC-11D0-A765-00A0C91E6BF6}"), &err);
Str s = uuid_string(u, a);
printf("%.*s\n", (int)s.len, s.p); /* f81d4fae-7dec-11d0-a765-00a0c91e6bf6 */

(void)uuid_parse_str(BURROW_S("f81d4fae-7dec-11d0-a765"), &err);
Str msg = error_text(err);
printf("%.*s\n", (int)msg.len, msg.p); /* invalid uuid */
```

`uuid_must_parse` panics instead of returning the error, for constants and tests. `uuid_nil` and `uuid_max` give the all zero and all one UUIDs, and a zeroed `Uuid` is the Nil one. Go compares UUIDs with `==`. C can't do that with a struct, so use `uuid_cmp(u, v) == 0`, which also gives the RFC's sort order.

## In JSON

The `Uuid` descriptor carries `MarshalText`, `UnmarshalText`, `AppendText` and `String`, so a `Uuid` field in a struct goes through `encoding/json` as its text and comes back from any form `uuid_parse_str` takes:

<!-- example: ../examples/uuid/uuid.c#decl -->
```c
#define ORDER_FIELDS(F, T)                                                             \
    F(T, Uuid, ID, "json:\"id\"")                                                      \
    F(T, Int, Qty, "json:\"qty\"")
BURROW_STRUCT(Order, ORDER_FIELDS);
```

<!-- example: ../examples/uuid/uuid.c#json -->
```c
Order o = {uuid_must_parse(BURROW_S("f81d4fae-7dec-11d0-a765-00a0c91e6bf6")),
           3};
Error err = BURROW_NO_ERROR;
Slice out = json_marshal(a, BURROW_ANY(TYPE_OF(Order), &o), &err);
printf("%.*s\n", (int)out.len, (const char *)out.p);
```

That prints `{"id":"f81d4fae-7dec-11d0-a765-00a0c91e6bf6","qty":3}`, the same as Go.

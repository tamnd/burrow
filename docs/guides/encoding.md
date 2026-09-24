# Encoding

`burrow/encoding.h` is Go's `encoding` package: the six interfaces a type implements to say how it turns into bytes or text and back. The encoders that come later, `encoding/json` and `encoding/xml` among them, look for these on every value they are given and use them in place of their own rules, the same way Go's do.

| Go | burrow method name | Signature macro |
|---|---|---|
| `BinaryMarshaler` | `MarshalBinary` | `ENCODING_SIG_MARSHAL_BINARY` |
| `BinaryUnmarshaler` | `UnmarshalBinary` | `ENCODING_SIG_UNMARSHAL_BINARY` |
| `BinaryAppender` | `AppendBinary` | `ENCODING_SIG_APPEND_BINARY` |
| `TextMarshaler` | `MarshalText` | `ENCODING_SIG_MARSHAL_TEXT` |
| `TextUnmarshaler` | `UnmarshalText` | `ENCODING_SIG_UNMARSHAL_TEXT` |
| `TextAppender` | `AppendText` | `ENCODING_SIG_APPEND_TEXT` |

## Implementing one

A type implements an interface by listing the method in its descriptor, the way a type becomes a Stringer for [fmt](fmt.md). The signature macro spells out the shape, so the method list reads the way Go's method set does:

<!-- example: ../examples/encoding/interfaces.c#declare -->
```c
#define POINT_FIELDS(F, T)                                                             \
    F(T, Int, X, "")                                                                   \
    F(T, Int, Y, "")
BURROW_STRUCT_DECL(Point, POINT_FIELDS);

static Slice point_marshal_text(Point *p, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str s = fmt_sprintf_v(a, "%d,%d", p->X, p->Y);
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Error point_unmarshal_text(Point *p, Alloc *a, Slice text) {
    (void)a;
    Str y;
    bool found;
    Str x = strings_cut(str_from_bytes(text.p, text.len), BURROW_S(","), &y, &found);
    if (!found)
        return errors_new(error_allocator(), BURROW_S("point: want x,y"));
    Error err = BURROW_NO_ERROR;
    p->X = strconv_atoi(x, &err);
    if (!BURROW_FAILED(err))
        p->Y = strconv_atoi(y, &err);
    return err;
}

#define POINT_METHODS(M, T)                                                            \
    M(T, MarshalText, point_marshal_text, ENCODING_SIG_MARSHAL_TEXT)                   \
    M(T, UnmarshalText, point_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)
BURROW_STRUCT_DEFINE_METHODS(Point, POINT_FIELDS, POINT_METHODS);
```

The methods take an allocator where Go's lean on the collector. The bytes a marshal method returns belong to the caller, and an unmarshal method copies anything it keeps out of the input, because the input is only borrowed. A method with the right name and a different signature does not count, which is Go's rule too.

## Using one

`encoding_marshal_text` and the other five calls find the method on an `Any` and call it. A pointer finds the methods of what it points at, and an interface value such as an `Error` finds the methods of the type inside it:

<!-- example: ../examples/encoding/interfaces.c#use -->
```c
Point p = {3, 4};
Any v = BURROW_ANY(TYPE_OF(Point), &p);
Error err = BURROW_NO_ERROR;
Slice text = encoding_marshal_text(a, v, &err);

Point q = {0, 0};
err = encoding_unmarshal_text(a, BURROW_ANY(TYPE_OF(Point), &q), BURROW_B("7,8"));
```

`text` is `3,4` and `q` is `{7 8}`. Go would write `v.(encoding.TextMarshaler)` and check the second result. Here that is `encoding_is_text_marshaler`, and calling the method on a value that lacks it returns the nil slice and Go's type assertion error instead of panicking:

<!-- example: ../examples/encoding/interfaces.c#missing -->
```c
Int n = 5;
Any iv = BURROW_ANY(TYPE_INT, &n);
bool ok = encoding_is_text_marshaler(iv);
encoding_marshal_text(a, iv, &err);
```

`ok` is false and the error reads `interface conversion: int is not encoding.TextMarshaler: missing method MarshalText`.

## Holding one

`EncodingTextMarshaler` and the other five are ordinary vtable interfaces, for code that wants to take one as a parameter or keep one in a struct, as described in [Interfaces](interfaces.md). An `Any` holding one of them, with `TYPE_OF(EncodingTextMarshaler)` as its type, works with the calls above, which go straight through the vtable.

## What is not here

Nothing from Go's `encoding` package is missing.

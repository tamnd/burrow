#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/encoding.h"
#include "burrow/mem/arena.h"

// doc: declare
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
// doc: end

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: use
    Point p = {3, 4};
    Any v = BURROW_ANY(TYPE_OF(Point), &p);
    Error err = BURROW_NO_ERROR;
    Slice text = encoding_marshal_text(a, v, &err);

    Point q = {0, 0};
    err = encoding_unmarshal_text(a, BURROW_ANY(TYPE_OF(Point), &q), BURROW_B("7,8"));
    // doc: end
    printf("text: %.*s\n", (int)text.len, (const char *)text.p);
    printf("q: %lld %lld\n", (long long)q.X, (long long)q.Y);

    // doc: missing
    Int n = 5;
    Any iv = BURROW_ANY(TYPE_INT, &n);
    bool ok = encoding_is_text_marshaler(iv);
    encoding_marshal_text(a, iv, &err);
    // doc: end
    printf("ok: %d\n", ok);
    printf("err: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
    arena_free(&ar);
    return 0;
}

/* Output:
text: 3,4
q: 7 8
ok: 0
err: interface conversion: int is not encoding.TextMarshaler: missing method MarshalText
*/

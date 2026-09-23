#include <stdio.h>

#include "burrow/burrow.h"

/* The declarations below repeat the ones in burrow/type.h. Repeating a
 * prototype is legal C, and the compiler rejects it if the two disagree, so the
 * guide cannot drift from the header. */

// doc: kinds
bool kind_is_signed(Kind k);   /* KIND_INT through KIND_INT64 */
bool kind_is_unsigned(Kind k); /* KIND_UINT through KIND_UINTPTR */
bool kind_is_float(Kind k);    /* KIND_FLOAT32 and KIND_FLOAT64 */
// doc: end

// doc: ops
bool type_equal(const Type *t, const void *a, const void *b);
uint64_t type_hash(const Type *t, const void *p, uint64_t seed);
void type_copy(const Type *t, void *dst, const void *src);
void type_zero(const Type *t, void *p);
// doc: end

// doc: comparable
bool type_is_comparable(const Type *t);
// doc: end

// doc: declare
#define POINT_FIELDS(F, T)                                                             \
    F(T, Int, X, "json:\"x\"")                                                         \
    F(T, Int, Y, "json:\"y\"")
BURROW_STRUCT(Point, POINT_FIELDS);
// doc: end

BURROW_SLICE_TYPE(IntSlice, Int);

static void size(void) {
    // doc: size
    const Type *t = TYPE_INT;
    printf("%u bytes, aligned to %u\n", t->size, t->align);
    // doc: end
}

static void kind(const Type *t) {
    // doc: kind
    if (t->kind == KIND_SLICE) {
        printf("a slice\n");
    }

    Str k = kind_name(t->kind); /* "slice" */
    // doc: end
    printf("kind " BURROW_STR_FMT "\n", BURROW_STR_ARG(k));
}

static void names(const Type *t) {
    // doc: name
    Str n = type_name(t);
    // doc: end

    // doc: pkg
    Str pkg = t->pkg_path; /* BURROW_S("image") for image.Point */
    // doc: end
    printf("name " BURROW_STR_FMT ", package \"" BURROW_STR_FMT "\"\n",
           BURROW_STR_ARG(n), BURROW_STR_ARG(pkg));
}

static void lookup(const Type *t) {
    // doc: lookup
    const Field *f = type_field_by_name(t, BURROW_S("X"));
    const Method *m = type_method_by_name(t, BURROW_S("String"));
    // doc: end
    printf("field " BURROW_STR_FMT " at offset %u, tag " BURROW_STR_FMT
           ", exported %d, embedded %d\n",
           BURROW_STR_ARG(f->name), f->offset, BURROW_STR_ARG(f->tag),
           field_is_exported(f), field_is_embedded(f));
    printf("String method %s\n", m == NULL ? "missing" : "found");
}

int main(void) {
    size();
    kind(TYPE_OF(IntSlice));
    kind(TYPE_INT);
    names(TYPE_INT);
    names(TYPE_OF(Point));
    names(TYPE_OF(IntSlice));
    lookup(TYPE_OF(Point));

    Point a = {3, 4}, b = {3, 4};
    printf("equal %d, comparable %d, signed %d\n", type_equal(TYPE_OF(Point), &a, &b),
           type_is_comparable(TYPE_OF(Point)), kind_is_signed(TYPE_INT->kind));
    return 0;
}

/* Output:
8 bytes, aligned to 8
a slice
kind slice
kind int
name int, package ""
name Point, package ""
name slice, package ""
field X at offset 0, tag json:"x", exported 1, embedded 0
String method missing
equal 1, comparable 1, signed 1
*/

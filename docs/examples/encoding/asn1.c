#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

// doc: declare
#define VALIDITY_FIELDS(F, T)                                                          \
    F(T, Time, NotBefore, "")                                                          \
    F(T, Time, NotAfter, "")
BURROW_STRUCT(Validity, VALIDITY_FIELDS);

#define RECORD_FIELDS(F, T)                                                            \
    F(T, Int, Version, "asn1:\"optional,explicit,default:0,tag:0\"")                   \
    F(T, Asn1ObjectIdentifier, Algorithm, "")                                          \
    F(T, Str, Name, "asn1:\"utf8\"")                                                   \
    F(T, Validity, Validity, "")
BURROW_STRUCT(Record, RECORD_FIELDS);
// doc: end

static void print_hex(Alloc *a, const char *label, Slice b) {
    Str h = hex_encode_to_string(a, b);
    printf("%s: " BURROW_STR_FMT "\n", label, BURROW_STR_ARG(h));
}

static void print_err(const char *label, Error err) {
    if (BURROW_FAILED(err)) {
        Str s = error_text(err);
        printf("%s: " BURROW_STR_FMT "\n", label, BURROW_STR_ARG(s));
    }
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    // doc: int
    Int n = 128;
    Slice der = asn1_marshal(a, BURROW_ANY(TYPE_INT, &n), &err);
    // doc: end
    print_hex(a, "128", der);

    // doc: marshal
    Record r = {
        .Version = 0,
        .Algorithm = ASN1_OID(1, 2, 840, 113549, 1, 1, 11),
        .Name = BURROW_S("gopher"),
        .Validity = {time_date(2026, TIME_JANUARY, 1, 0, 0, 0, 0, time_utc_loc),
                     time_date(2027, TIME_JANUARY, 1, 0, 0, 0, 0, time_utc_loc)},
    };
    der = asn1_marshal(a, BURROW_ANY(TYPE_OF(Record), &r), &err);
    // doc: end
    print_err("marshal", err);
    print_hex(a, "record", der);

    // doc: unmarshal
    Record got = {0};
    Slice rest = asn1_unmarshal(a, der, BURROW_ANY(TYPE_OF(Record), &got), &err);
    // doc: end
    print_err("unmarshal", err);
    Str oid = asn1_object_identifier_string(got.Algorithm, a);
    Str after = time_format(got.Validity.NotAfter, a, TIME_RFC3339);
    printf("name " BURROW_STR_FMT ", oid " BURROW_STR_FMT ", until " BURROW_STR_FMT
           ", %d bytes left\n",
           BURROW_STR_ARG(got.Name), BURROW_STR_ARG(oid), BURROW_STR_ARG(after),
           (int)rest.len);

    // doc: raw
    Asn1RawValue raw = {0};
    asn1_unmarshal(a, der, BURROW_ANY(TYPE_ASN1_RAW_VALUE, &raw), &err);
    // doc: end
    print_err("raw", err);
    printf("class %d, tag %d, compound %d, %d bytes inside\n", (int)raw.cls,
           (int)raw.tag, raw.is_compound, (int)raw.bytes.len);

    // doc: bad
    Str s = BURROW_S("x");
    Slice trunc = slice_sub(der, 0, 10);
    asn1_unmarshal(a, trunc, BURROW_ANY(TYPE_OF(Record), &got), &err);
    print_err("truncated", err);
    asn1_unmarshal(a, der, BURROW_ANY(TYPE_STRING, &s), &err);
    // doc: end
    print_err("into a string", err);

    arena_free(&ar);
    return 0;
}

/* Output:
128: 02020080
record: 303306092a864886f70d01010b0c06676f70686572301e170d3236303130313030303030305a170d3237303130313030303030305a
name gopher, oid 1.2.840.113549.1.1.11, until 2027-01-01T00:00:00Z, 0 bytes left
class 0, tag 16, compound 1, 51 bytes inside
truncated: asn1: syntax error: data truncated
into a string: asn1: structure error: tags don't match (19 vs {class:0 tag:16 length:51 isCompound:true}) {optional:false explicit:false application:false private:false defaultValue:<nil> tag:<nil> stringType:0 timeType:0 set:false omitEmpty:false} string @2
*/

#include <stdio.h>

#include "burrow/burrow.h"

static void print(Str s) {
    printf("%.*s\n", (int)s.len, s.p);
}

static void names(Alloc *a) {
    // doc: name
    Str org = BURROW_S("Example Ltd");
    PkixName n = {0};
    n.common_name = BURROW_S("www.example.com");
    n.organization = slice_append(a, slice_nil(TYPE_STRING), &org, 1);
    PkixRDNSequence rdns = pkix_name_to_rdn_sequence(n, a);

    Error err = BURROW_NO_ERROR;
    Slice der = asn1_marshal(a, BURROW_ANY(TYPE_PKIX_RDN_SEQUENCE, &rdns), &err);
    print(hex_encode_to_string(a, der));
    // doc: end

    // doc: parse
    PkixRDNSequence back = slice_nil(TYPE_PKIX_RELATIVE_DISTINGUISHED_NAME_SET);
    asn1_unmarshal(a, der, BURROW_ANY(TYPE_PKIX_RDN_SEQUENCE, &back), &err);
    if (BURROW_FAILED(err))
        return;
    PkixName parsed = {0};
    pkix_name_fill_from_rdn_sequence(&parsed, a, &back);
    print(pkix_name_string(parsed, a));
    print(parsed.common_name);
    printf("%d attributes\n", (int)parsed.names.len);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    names(arena_allocator(&ar));
    arena_free(&ar);
    return 0;
}

/* Output:
303031143012060355040a130b4578616d706c65204c7464311830160603550403130f7777772e6578616d706c652e636f6d
CN=www.example.com,O=Example Ltd
www.example.com
2 attributes
*/

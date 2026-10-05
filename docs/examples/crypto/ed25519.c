#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

static Slice text(const char *s) {
    Int n = (Int)strlen(s);
    return slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static void print_hex(Alloc *a, Slice b) {
    Str s = hex_encode_to_string(a, b);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

static void print_error(Error err) {
    if (BURROW_OK(err)) {
        printf("ok\n");
        return;
    }
    Str s = error_text(err);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

static void generate(Alloc *a) {
    // doc: generate
    Error err = BURROW_NO_ERROR;
    Ed25519PrivateKey priv;
    Ed25519PublicKey pub = ed25519_generate_key(a, (IoReader){NULL, NULL}, &priv, &err);
    if (BURROW_FAILED(err))
        return;
    // doc: end
    printf("%d %d\n", (int)pub.len, (int)priv.len);
}

static void sign(Alloc *a) {
    // doc: sign
    Ed25519PrivateKey priv =
        ed25519_new_key_from_seed(a, text("an ed25519 seed is 32 bytes long"));
    Ed25519PublicKey pub = ed25519_private_key_public(priv, a);

    Slice msg = text("The quick brown fox jumps over the lazy dog");
    Slice sig = ed25519_sign(a, priv, msg);
    // doc: end
    print_hex(a, pub);
    print_hex(a, sig);

    // doc: verify
    printf("%d\n", ed25519_verify(pub, msg, sig));
    printf("%d\n", ed25519_verify(
                       pub, text("The quick brown fox jumps over the lazy cat"), sig));
    // doc: end

    // doc: context
    Error err = BURROW_NO_ERROR;
    Ed25519Options opts = {0, BURROW_S("Example_ed25519ctx")};
    Slice ctx_sig =
        ed25519_private_key_sign(priv, a, (IoReader){NULL, NULL}, msg,
                                 ed25519_options_as_signer_opts(&opts), &err);
    if (BURROW_FAILED(err))
        return;
    print_error(ed25519_verify_with_options(pub, msg, ctx_sig, &opts));

    Ed25519Options other = {0, BURROW_S("another context")};
    print_error(ed25519_verify_with_options(pub, msg, ctx_sig, &other));
    // doc: end
    print_hex(a, ctx_sig);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    generate(a);
    sign(a);
    arena_free(&ar);
    return 0;
}

/* Output:
32 64
6def0b1dcdf1694a2f4ed27f9050ccb27673c5c685ee9919c1c38fa6fada4406
906b0482114437fb61868fdd0554b518b96613bc8a570f20a3780dbb8b75d167fc06e07ef08ed4f0d865bcfefdf612d9897e0d3faed3c99c08fb2b8f9cb14a0e
1
0
ok
ed25519: invalid signature
ee98cec2603c88a9e7c9627d7ee12c8fafed89828a8ea45ff1c37d84488bbc6e48cb10db51fb2d7af9da6f7f1d23c709bba4953ad22c2aa578b26ca6274f0201
*/

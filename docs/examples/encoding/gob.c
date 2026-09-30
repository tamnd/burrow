#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/encoding/gob.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/arena.h"

// doc: declare
BURROW_SLICE_TYPE(StrSlice, Str);

#define ITEM_FIELDS(F, T)                                                              \
    F(T, Str, Name, "")                                                                \
    F(T, Int, Count, "")                                                               \
    F(T, double, Price, "")                                                            \
    F(T, StrSlice, Tags, "")
BURROW_STRUCT(Item, ITEM_FIELDS);
// doc: end

// doc: subset
#define LABEL_FIELDS(F, T)                                                             \
    F(T, Str, Name, "")                                                                \
    F(T, Int, Count, "")                                                               \
    F(T, Str, Note, "")
BURROW_STRUCT(Label, LABEL_FIELDS);
// doc: end

// doc: method
#define VERSION_FIELDS(F, T)                                                           \
    F(T, Int, Major, "")                                                               \
    F(T, Int, Minor, "")
BURROW_STRUCT_DECL(Version, VERSION_FIELDS);

static Slice version_gob_encode(Version *v, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Byte two[2] = {(Byte)v->Major, (Byte)v->Minor};
    return slice_append(a, slice_nil(TYPE_BYTE), two, 2);
}

static Error version_gob_decode(Version *v, Alloc *a, Slice b) {
    (void)a;
    if (b.len != 2)
        return errors_new(error_allocator(), BURROW_S("version: want two bytes"));
    v->Major = ((const Byte *)b.p)[0];
    v->Minor = ((const Byte *)b.p)[1];
    return BURROW_NO_ERROR;
}

#define VERSION_METHODS(M, T)                                                          \
    M(T, GobDecode, version_gob_decode, GOB_SIG_GOB_DECODE)                            \
    M(T, GobEncode, version_gob_encode, GOB_SIG_GOB_ENCODE)
BURROW_STRUCT_DEFINE_METHODS(Version, VERSION_FIELDS, VERSION_METHODS);
// doc: end

#define BOX_FIELDS(F, T) F(T, Any, Value, "")
BURROW_STRUCT(Box, BOX_FIELDS);

static void print_err(const char *what, Error err) {
    Str s = BURROW_FAILED(err) ? error_text(err) : BURROW_S("<nil>");
    printf("%s: %.*s\n", what, (int)s.len, (const char *)s.p);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: encode
    Str tags[2] = {BURROW_S("red"), BURROW_S("small")};
    Item it = {BURROW_S("widget"), 3, 2.5, slice_from(tags, 2, 2, TYPE_STRING)};
    BytesBuffer buf = BYTES_BUFFER(a);
    GobEncoder *enc = gob_new_encoder(a, bytes_buffer_as_io_writer(&buf));
    Error err = gob_encoder_encode(enc, BURROW_ANY(TYPE_OF(Item), &it));
    // doc: end
    print_err("encode", err);
    Str hx = hex_encode_to_string(a, bytes_buffer_bytes(&buf));
    printf("%lld bytes: %.*s\n", (long long)bytes_buffer_len(&buf), (int)hx.len,
           (const char *)hx.p);

    // doc: second
    it.Count = 4;
    Int before = bytes_buffer_len(&buf);
    err = gob_encoder_encode(enc, BURROW_ANY(TYPE_OF(Item), &it));
    gob_encoder_free(enc);
    // doc: end
    print_err("encode again", err);
    printf("second value: %lld bytes\n", (long long)(bytes_buffer_len(&buf) - before));

    // doc: decode
    GobDecoder *dec = gob_new_decoder(a, bytes_buffer_as_io_reader(&buf));
    Item got = {0};
    err = gob_decoder_decode(dec, BURROW_ANY(TYPE_OF(Item), &got));
    // doc: end
    print_err("decode", err);
    printf("%.*s count %lld price %g tags %lld\n", (int)got.Name.len,
           (const char *)got.Name.p, (long long)got.Count, got.Price,
           (long long)got.Tags.len);

    // doc: into
    Label l = {0};
    err = gob_decoder_decode(dec, BURROW_ANY(TYPE_OF(Label), &l));
    // doc: end
    print_err("decode into Label", err);
    printf("label %.*s count %lld note %lld bytes\n", (int)l.Name.len,
           (const char *)l.Name.p, (long long)l.Count, (long long)l.Note.len);
    err = gob_decoder_decode(dec, BURROW_ANY(TYPE_OF(Label), &l));
    print_err("at the end", err);
    gob_decoder_free(dec);

    // doc: iface
    gob_register(BURROW_ANY(TYPE_OF(Version), NULL));
    Version v = {1, 22};
    Box box = {BURROW_ANY(TYPE_OF(Version), &v)};
    BytesBuffer vb = BYTES_BUFFER(a);
    enc = gob_new_encoder(a, bytes_buffer_as_io_writer(&vb));
    err = gob_encoder_encode(enc, BURROW_ANY(TYPE_OF(Box), &box));
    gob_encoder_free(enc);
    // doc: end
    print_err("encode a Box", err);

    // doc: iface-decode
    dec = gob_new_decoder(a, bytes_buffer_as_io_reader(&vb));
    Box out = {0};
    err = gob_decoder_decode(dec, BURROW_ANY(TYPE_OF(Box), &out));
    gob_decoder_free(dec);
    // doc: end
    print_err("interface", err);
    if (out.Value.t == TYPE_OF(Version)) {
        Version *w = out.Value.data;
        printf("version %lld.%lld\n", (long long)w->Major, (long long)w->Minor);
    }

    BytesBuffer ib = BYTES_BUFFER(a);
    enc = gob_new_encoder(a, bytes_buffer_as_io_writer(&ib));
    err = gob_encoder_encode(enc, BURROW_ANY(TYPE_OF(Item), &it));
    gob_encoder_free(enc);
    print_err("encode an Item", err);

    // doc: bad
    Int n = 0;
    dec = gob_new_decoder(a, bytes_buffer_as_io_reader(&ib));
    err = gob_decoder_decode(dec, BURROW_ANY(TYPE_INT, &n));
    gob_decoder_free(dec);
    // doc: end
    print_err("into an Int", err);

    arena_free(&ar);
    return 0;
}

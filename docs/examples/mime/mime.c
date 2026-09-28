#include "burrow/mime.h"
#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

/* A charset reader for windows-1252 that only knows the euro sign, enough to
 * show the hook. What it makes comes from the arena in env. */
typedef struct Converter {
    Alloc *a;
    StringsReader out;
} Converter;

static IoReader cp1252(void *env, Str charset, IoReader input, Error *err) {
    Converter *cv = (Converter *)env;
    if (!str_eq(charset, BURROW_S("windows-1252"))) {
        *err = fmt_errorf_v("no reader for %q", charset);
        return (IoReader){0};
    }
    Slice in = io_read_all(cv->a, input, err);
    StringsBuilder b = STRINGS_BUILDER(cv->a);
    for (Int i = 0; i < in.len; i++) {
        Byte c = ((const Byte *)in.p)[i];
        if (c == 0x80)
            strings_builder_write_rune(&b, 0x20AC, err);
        else
            strings_builder_write_byte(&b, c);
    }
    strings_reader_reset(&cv->out, strings_builder_string(&b));
    return strings_reader_as_io_reader(&cv->out);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;

    // doc: parse
    Map *params;
    Str t = mime_parse_media_type(
        a, BURROW_S("Text/HTML; Charset=\"UTF-8\"; title*=utf-8''caf%C3%A9"), &params,
        &err);
    Str charset = BURROW_S("charset"), title = BURROW_S("title");
    fmt_printf_v("type %s, charset %s, title %s\n", t,
                 *(const Str *)map_get(params, &charset),
                 *(const Str *)map_get(params, &title));

    t = mime_parse_media_type(a, BURROW_S("text/plain; oops"), &params, &err);
    fmt_printf_v("type %s, error: %v\n", t, err);
    // doc: end

    // doc: format
    Map *m = map_make(a, TYPE_STRING, TYPE_STRING, 2);
    Str k1 = BURROW_S("filename"), v1 = BURROW_S("r\xc3\xa9sum\xc3\xa9.pdf");
    Str k2 = BURROW_S("Size"), v2 = BURROW_S("1024");
    map_set(m, &k1, &v1);
    map_set(m, &k2, &v2);
    fmt_printf_v("%s\n", mime_format_media_type(a, BURROW_S("attachment"), m));
    // doc: end

    // doc: encode
    Str subject = BURROW_S("Caf\xc3\xa9 au lait");
    fmt_printf_v("%s\n", mime_word_encoder_encode(MIME_Q_ENCODING, a, BURROW_S("utf-8"),
                                                  subject));
    fmt_printf_v("%s\n", mime_word_encoder_encode(MIME_B_ENCODING, a, BURROW_S("utf-8"),
                                                  subject));
    // doc: end

    // doc: decode
    MimeWordDecoder dec = {0};
    Str h = mime_word_decoder_decode_header(
        &dec, a, BURROW_S("Re: =?utf-8?q?Caf=C3=A9?= =?iso-8859-1?b?YXUgbGFpdA==?="),
        &err);
    fmt_printf_v("%s\n", h);
    // doc: end

    // doc: charset
    Converter cv = {a, {0}};
    MimeWordDecoder custom = {BURROW_FN(MimeCharsetReader, cp1252, &cv)};
    h = mime_word_decoder_decode_header(
        &custom, a, BURROW_S("Price: =?Windows-1252?q?=80_5?="), &err);
    fmt_printf_v("%s\n", h);
    h = mime_word_decoder_decode_header(&custom, a, BURROW_S("=?koi8-r?q?x?="), &err);
    fmt_printf_v("error: %v\n", err);
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
type text/html, charset UTF-8, title café
type text/plain, error: mime: invalid media parameter
attachment; size=1024; filename*=utf-8''r%C3%A9sum%C3%A9.pdf
=?utf-8?q?Caf=C3=A9_au_lait?=
=?utf-8?b?Q2Fmw6kgYXUgbGFpdA==?=
Re: Caféau lait
Price: € 5
error: no reader for "koi8-r"
*/

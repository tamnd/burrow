#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/encoding/xml.h"
#include "burrow/mem/arena.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: read
    StringsReader sr;
    strings_reader_reset(&sr, BURROW_S("<feed xmlns=\"http://www.w3.org/2005/Atom\">"
                                       "<title>Burrow &amp; friends</title>"
                                       "<link href=\"https://example.com/\"/>"
                                       "</feed>"));
    XmlDecoder *d = xml_new_decoder(a, strings_reader_as_io_reader(&sr));
    for (;;) {
        Error err = BURROW_NO_ERROR;
        XmlToken tok = xml_decoder_token(d, &err);
        if (errors_is(err, io_eof))
            break;
        if (tok.kind == XML_START_ELEMENT) {
            Str space = tok.start.name.space, local = tok.start.name.local;
            fmt_printf_v("start %s in %s, %d attributes\n", local, space,
                         tok.start.attr.len);
        } else if (tok.kind == XML_CHAR_DATA) {
            Slice text = tok.char_data;
            fmt_printf_v("text %q\n", text);
        } else if (tok.kind == XML_END_ELEMENT) {
            Str local = tok.end.name.local;
            fmt_printf_v("end %s\n", local);
        }
    }
    xml_decoder_free(d);
    // doc: end

    // doc: html
    strings_reader_reset(&sr, BURROW_S("<p>caf&eacute;<br>menu</p>"));
    d = xml_new_decoder(a, strings_reader_as_io_reader(&sr));
    d->strict = false;
    d->auto_close = xml_html_auto_close;
    d->entity = xml_html_entity();
    Error err = BURROW_NO_ERROR;
    Int n = 0;
    for (xml_decoder_token(d, &err); BURROW_OK(err); xml_decoder_token(d, &err))
        n++;
    xml_decoder_free(d);
    // doc: end
    printf("%d tokens, then %s\n", (int)n, errors_is(err, io_eof) ? "EOF" : "an error");

    // doc: bad
    strings_reader_reset(&sr, BURROW_S("<a>\n<b></a>"));
    d = xml_new_decoder(a, strings_reader_as_io_reader(&sr));
    for (xml_decoder_token(d, &err); BURROW_OK(err); xml_decoder_token(d, &err)) {
    }
    const XmlSyntaxError *se = errors_as(err, TYPE_XML_SYNTAX_ERROR);
    xml_decoder_free(d);
    // doc: end
    printf("line %d: " BURROW_STR_FMT "\n", (int)se->line, BURROW_STR_ARG(se->msg));
    printf("err: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));

    // doc: write
    BytesBuffer out = BYTES_BUFFER(a);
    XmlEncoder *e = xml_new_encoder(a, bytes_buffer_as_io_writer(&out));
    xml_encoder_indent(e, BURROW_S(""), BURROW_S("  "));
    XmlAttr lang[] = {{{BURROW_S(""), BURROW_S("lang")}, BURROW_S("en")}};
    XmlStartElement note = {{BURROW_S(""), BURROW_S("note")}, {0}};
    XmlStartElement body = {{BURROW_S(""), BURROW_S("body")},
                            slice_from(lang, 1, 1, &burrow_type_XmlAttr)};
    xml_encoder_encode_token(e, (XmlToken){XML_START_ELEMENT, .start = note});
    xml_encoder_encode_token(e, (XmlToken){XML_START_ELEMENT, .start = body});
    xml_encoder_encode_token(e,
                             (XmlToken){XML_CHAR_DATA, .char_data = BURROW_B("1 < 2")});
    xml_encoder_encode_token(
        e, (XmlToken){XML_END_ELEMENT, .end = xml_start_element_end(body)});
    xml_encoder_encode_token(
        e, (XmlToken){XML_END_ELEMENT, .end = xml_start_element_end(note)});
    err = xml_encoder_close(e);
    xml_encoder_free(e);
    // doc: end
    Str text = bytes_buffer_string(&out, a);
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(text));
    printf("err: %d\n", BURROW_FAILED(err));

    arena_free(&ar);
    return 0;
}

/* Output:
start feed in http://www.w3.org/2005/Atom, 1 attributes
start title in http://www.w3.org/2005/Atom, 0 attributes
text "Burrow & friends"
end title
start link in http://www.w3.org/2005/Atom, 1 attributes
end link
end feed
6 tokens, then EOF
line 2: element <b> closed by </a>
err: XML syntax error on line 2: element <b> closed by </a>
<note>
  <body lang="en">1 &lt; 2</body>
</note>
err: 0
*/

#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/encoding/json/jsontext.h"
#include "burrow/mem/arena.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: read
    StringsReader sr;
    strings_reader_reset(&sr,
                         BURROW_S("{\"name\": \"gopher\", \"tags\": [\"go\", 1.5e3]}"));
    JsontextDecoder *d = jsontext_new_decoder_v(a, strings_reader_as_io_reader(&sr), 0);
    for (;;) {
        Error err = BURROW_NO_ERROR;
        JsontextToken tok = jsontext_decoder_read_token(d, &err);
        if (errors_is(err, io_eof))
            break;
        JsontextPointer at = jsontext_decoder_stack_pointer(d, a);
        Str kind = jsontext_kind_string(jsontext_token_kind(tok));
        fmt_printf_v("%-8s %-10s %s\n", kind, at, jsontext_token_string(tok, a));
    }
    jsontext_decoder_free(d);
    // doc: end

    // doc: bad
    strings_reader_reset(&sr, BURROW_S("{\"a\": 1, \"a\": 2}"));
    d = jsontext_new_decoder_v(a, strings_reader_as_io_reader(&sr), 0);
    Error err = BURROW_NO_ERROR;
    jsontext_decoder_read_value(d, &err);
    const JsontextSyntacticError *se = errors_as(err, TYPE_JSONTEXT_SYNTACTIC_ERROR);
    // doc: end
    printf("offset %d, pointer " BURROW_STR_FMT ", duplicate: %d\n",
           (int)se->byte_offset, BURROW_STR_ARG(se->json_pointer),
           errors_is(err, jsontext_err_duplicate_name));
    printf("err: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
    jsontext_decoder_free(d);

    // doc: write
    BytesBuffer out = BYTES_BUFFER(a);
    JsontextEncoder *e = jsontext_new_encoder_v(a, bytes_buffer_as_io_writer(&out), 1,
                                                jsontext_with_indent(BURROW_S("  ")));
    jsontext_encoder_write_token(e, jsontext_begin_object);
    jsontext_encoder_write_token(e, jsontext_string(BURROW_S("name")));
    jsontext_encoder_write_token(e, jsontext_string(BURROW_S("<gopher>")));
    jsontext_encoder_write_token(e, jsontext_string(BURROW_S("sizes")));
    jsontext_encoder_write_value(e, BURROW_B("[1, 2.50, 1e3]"));
    err = jsontext_encoder_write_token(e, jsontext_end_object);
    jsontext_encoder_free(e);
    // doc: end
    Str text = bytes_buffer_string(&out, a);
    printf(BURROW_STR_FMT, BURROW_STR_ARG(text));
    printf("err: %d\n", BURROW_FAILED(err));

    // doc: value
    JsontextValue v = jsontext_value_clone(
        BURROW_B("{\"b\": 2.0, \"a\": [true, 1E2], \"\\u00e9\": \"x\"}"), a);
    Error cerr = jsontext_value_canonicalize_v(&v, a, 0);
    // doc: end
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(jsontext_value_string(v)));
    printf("err: %d\n", BURROW_FAILED(cerr));

    arena_free(&ar);
    return 0;
}

/* Output:
{                   {
string   /name      name
string   /name      gopher
string   /tags      tags
[        /tags      [
string   /tags/0    go
number   /tags/1    1.5e3
]        /tags      ]
}                   }
offset 9, pointer /a, duplicate: 1
err: jsontext: duplicate object member name "a"
{
  "name": "<gopher>",
  "sizes": [
    1,
    2.50,
    1e3
  ]
}
err: 0
{"a":[true,100],"b":2,"é":"x"}
err: 0
*/

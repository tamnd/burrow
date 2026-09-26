#include "burrow/burrow.h"

static void tokens(Alloc *a) {
    // doc: tokens
    StringsReader r;
    strings_reader_reset(&r, BURROW_S("\n// This is scanned code.\nif a > 10 {\n"
                                      "\tsomeParsable = text\n}"));
    TextScanner s = {0};
    text_scanner_init(&s, strings_reader_as_io_reader(&r));
    s.position.filename = BURROW_S("example");
    Rune tok;
    while ((tok = text_scanner_scan(&s)) != TEXT_SCANNER_EOF)
        fmt_printf_v("%s: %s\n", text_scanner_position_string(s.position, a),
                     text_scanner_token_text(&s));
    text_scanner_free(&s);
    // doc: end
}

static void kinds(Alloc *a) {
    // doc: kinds
    StringsReader r;
    strings_reader_reset(&r, BURROW_S("x := 3.5e2 + 'a' // half\nname = `raw` \"q\""));
    TextScanner s = {0};
    text_scanner_init(&s, strings_reader_as_io_reader(&r));
    Rune tok;
    while ((tok = text_scanner_scan(&s)) != TEXT_SCANNER_EOF)
        fmt_printf_v("%-8s %s\n", text_scanner_token_string(a, tok),
                     text_scanner_token_text(&s));
    text_scanner_free(&s);
    // doc: end
}

// doc: report
static void report(void *env, TextScanner *s, Str msg) {
    Alloc *a = env;
    fmt_printf_v("error at %s: %s\n",
                 text_scanner_position_string(text_scanner_pos(s), a), msg);
}
// doc: end

static void errors(Alloc *a) {
    // doc: errors
    StringsReader r;
    strings_reader_reset(&r, BURROW_S("ok 0x \"open"));
    TextScanner s = {0};
    text_scanner_init(&s, strings_reader_as_io_reader(&r));
    s.position.filename = BURROW_S("input");
    s.error = BURROW_FN(TextScannerErrorFunc, report, a);
    while (text_scanner_scan(&s) != TEXT_SCANNER_EOF) {
    }
    fmt_println_v("errors:", s.error_count);
    text_scanner_free(&s);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    tokens(a);
    kinds(a);
    errors(a);
    arena_free(&ar);
    return 0;
}

/* Output:
example:3:1: if
example:3:4: a
example:3:6: >
example:3:8: 10
example:3:11: {
example:4:2: someParsable
example:4:15: =
example:4:17: text
example:5:1: }
Ident    x
":"      :
"="      =
Float    3.5e2
"+"      +
Char     'a'
Ident    name
"="      =
RawString `raw`
String   "q"
error at input:1:6: hexadecimal literal has no digits
error at input:1:12: literal not terminated
errors: 2
*/

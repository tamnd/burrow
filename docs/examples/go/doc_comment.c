#include "burrow/burrow.h"

static void parse(Alloc *a) {
    // doc: parse
    Str text = BURROW_S("Package hello says hello. See [strings.TrimSpace].\n"
                        "\n"
                        "# Usage\n"
                        "\n"
                        "Call it like this:\n"
                        "\n"
                        "\tfmt.Println(hello.Greet(\"world\"))\n"
                        "\n"
                        "It knows two greetings:\n"
                        "  - hello\n"
                        "  - goodbye\n");
    CommentDoc *d = comment_parser_parse(NULL, a, text);
    for (Int i = 0; i < d->content.len; i++) {
        CommentBlock b = ((CommentBlock *)d->content.p)[i];
        switch ((int)b->kind) {
        case COMMENT_KIND_PARAGRAPH:
            fmt_printf_v("paragraph of %d text(s)\n",
                         ((CommentParagraph *)b)->text.len);
            break;
        case COMMENT_KIND_HEADING:
            fmt_printf_v("heading, id %s\n",
                         comment_heading_default_id((CommentHeading *)b, a));
            break;
        case COMMENT_KIND_CODE:
            fmt_printf_v("code %q\n", ((CommentCode *)b)->text);
            break;
        case COMMENT_KIND_LIST:
            fmt_printf_v("list of %d item(s)\n", ((CommentList *)b)->items.len);
            break;
        default:
            break;
        }
    }
    // doc: end
}

static void print(Alloc *a) {
    // doc: print
    Str text = BURROW_S("Greet returns a greeting for name, as in\n"
                        "\"hello, name\". It trims the name with [strings.TrimSpace]\n"
                        "first, and an empty name gets a greeting all the same.\n");
    CommentDoc *d = comment_parser_parse(NULL, a, text);
    CommentPrinter p = {0};
    p.doc_link_base_url = BURROW_S("https://pkg.go.dev");
    p.text_width = 40;
    Slice out = comment_printer_text(&p, a, d);
    fmt_printf_v("%s", str_from_bytes(out.p, out.len));
    out = comment_printer_markdown(&p, a, d);
    fmt_printf_v("%s", str_from_bytes(out.p, out.len));
    out = comment_printer_html(&p, a, d);
    fmt_printf_v("%s", str_from_bytes(out.p, out.len));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    parse(arena_allocator(&ar));
    print(arena_allocator(&ar));
    arena_free(&ar);
    return 0;
}

/* Output:
*/

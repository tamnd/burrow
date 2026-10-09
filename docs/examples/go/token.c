#include "burrow/burrow.h"

static void positions(Alloc *a) {
    // doc: positions
    Str src = BURROW_S("package main\n\nfunc main() {\n\tprintln(\"hi\")\n}\n");
    TokenFileSet *fset = token_new_file_set(a);
    TokenFile *f = token_file_set_add_file(fset, BURROW_S("main.go"), -1, src.len);
    token_file_set_lines_for_content(f, slice_from_str(a, src));

    TokenPos p = token_file_pos(f, strings_index(src, BURROW_S("println")));
    TokenPosition pos = token_file_set_position(fset, p);
    fmt_println_v(p, token_position_string(pos, a));

    TokenFile *g = token_file_set_add_file(fset, BURROW_S("util.go"), -1, 100);
    fmt_println_v(token_file_string(f, a), token_file_string(g, a));
    fmt_println_v(token_file_name(token_file_set_file(fset, token_file_pos(g, 7))));
    token_file_set_free(fset);
    // doc: end
}

static void directives(Alloc *a) {
    // doc: directives
    Str src = BURROW_S("a\nb\nc\nd\n");
    TokenFileSet *fset = token_new_file_set(a);
    TokenFile *f = token_file_set_add_file(fset, BURROW_S("gen.go"), -1, src.len);
    token_file_set_lines_for_content(f, slice_from_str(a, src));

    // From line 3 on, positions are reported as lines of parser.y from 40.
    token_file_add_line_info(f, 4, BURROW_S("parser.y"), 40);

    TokenPos p = token_file_line_start(f, 4);
    fmt_println_v(token_position_string(token_file_set_position(fset, p), a));
    fmt_println_v(
        token_position_string(token_file_set_position_for(fset, p, false), a));
    token_file_set_free(fset);
    // doc: end
}

static void tokens(Alloc *a) {
    // doc: tokens
    Str words[] = {BURROW_S("func"), BURROW_S("Func"), BURROW_S("x1"),
                   BURROW_S("1x")};
    for (int i = 0; i < 4; i++) {
        Token tok = token_lookup(words[i]);
        fmt_printf_v("%-5s %-6s identifier=%t exported=%t\n", words[i],
                     token_string(tok, a), token_is_identifier(words[i]),
                     token_is_exported(words[i]));
    }
    Token ops[] = {TOKEN_LOR, TOKEN_EQL, TOKEN_ADD, TOKEN_MUL, TOKEN_ARROW};
    for (int i = 0; i < 5; i++)
        fmt_printf_v("%s binds at %d\n", token_string(ops[i], a),
                     token_precedence(ops[i]));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    positions(a);
    directives(a);
    tokens(a);
    arena_free(&ar);
    return 0;
}

/* Output:
30 main.go:4:2
main.go(1-46) util.go(47-147)
util.go
parser.y:41:1
gen.go:4:1
func  func   identifier=false exported=false
Func  IDENT  identifier=true exported=true
x1    IDENT  identifier=true exported=false
1x    IDENT  identifier=false exported=false
|| binds at 1
== binds at 3
+ binds at 4
* binds at 5
<- binds at 0
*/

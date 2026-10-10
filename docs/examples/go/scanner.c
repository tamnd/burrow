#include "burrow/burrow.h"

static void scan(Alloc *a) {
    // doc: scan
    Str src = BURROW_S("cos(x) + 1i*sin(x) // Euler");
    TokenFileSet *fset = token_new_file_set(a);
    TokenFile *file = token_file_set_add_file(fset, BURROW_S(""), -1, src.len);
    GoScanner s;
    go_scanner_init(&s, a, file, slice_from_str(a, src), (GoScannerErrorHandler){0},
                    GO_SCANNER_SCAN_COMMENTS);
    for (;;) {
        Token tok;
        Str lit;
        TokenPos pos = go_scanner_scan(&s, &tok, &lit);
        if (tok == TOKEN_EOF)
            break;
        Str where = token_position_string(token_file_set_position(fset, pos), a);
        fmt_printf_v("%s\t%s\t%q\n", where, token_string(tok, a), lit);
    }
    token_file_set_free(fset);
    // doc: end
}

// doc: handler
typedef struct Collect {
    GoScannerErrorList list;
    Alloc *a;
} Collect;

static void collect(void *env, TokenPosition pos, Str msg) {
    Collect *c = env;
    go_scanner_error_list_add(&c->list, c->a, pos, msg);
}
// doc: end

static void errors(Alloc *a) {
    // doc: errors
    Str src = BURROW_S("z := \"open\ny := 0x\nx := 'ab'\n");
    TokenFileSet *fset = token_new_file_set(a);
    TokenFile *file = token_file_set_add_file(fset, BURROW_S("bad.go"), -1, src.len);
    Collect c = {{0}, a};
    GoScanner s;
    go_scanner_init(&s, a, file, slice_from_str(a, src),
                    BURROW_FN(GoScannerErrorHandler, collect, &c), 0);
    Token tok;
    do
        go_scanner_scan(&s, &tok, NULL);
    while (tok != TOKEN_EOF);

    go_scanner_error_list_sort(c.list);
    Error err = go_scanner_error_list_err(c.list);
    fmt_println_v(err);
    BytesBuffer out = BYTES_BUFFER(a);
    go_scanner_print_error(bytes_buffer_as_io_writer(&out), err);
    fmt_printf_v("%s", bytes_buffer_string(&out, a));
    token_file_set_free(fset);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    scan(a);
    errors(a);
    arena_free(&ar);
    return 0;
}

/* Output:
1:1	IDENT	"cos"
1:4	(	""
1:5	IDENT	"x"
1:6	)	""
1:8	+	""
1:10	IMAG	"1i"
1:12	*	""
1:13	IDENT	"sin"
1:16	(	""
1:17	IDENT	"x"
1:18	)	""
1:20	COMMENT	"// Euler"
1:28	;	"\n"
bad.go:1:6: string literal not terminated (and 2 more errors)
bad.go:1:6: string literal not terminated
bad.go:2:8: hexadecimal literal has no digits
bad.go:3:6: illegal rune literal
*/

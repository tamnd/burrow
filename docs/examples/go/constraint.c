#include "burrow/burrow.h"

/* Says yes to the tags a linux/amd64 build with Go 1.21 has. A real build also
 * says yes to every older go1.N, which this leaves out. */
static bool linux_amd64(void *env, Str tag) {
    (void)env;
    return str_eq(tag, BURROW_S("linux")) || str_eq(tag, BURROW_S("amd64")) ||
           str_eq(tag, BURROW_S("go1.21"));
}

static void parse(Alloc *a) {
    // doc: parse
    Str lines[] = {
        BURROW_S("//go:build linux && (amd64 || arm64)"),
        BURROW_S("//go:build !windows && go1.21"),
        BURROW_S("// +build darwin,!cgo freebsd"),
        BURROW_S("//go:build linux &&"),
        BURROW_S("// just a comment"),
    };
    ConstraintTagFunc ok = BURROW_FN(ConstraintTagFunc, linux_amd64, NULL);
    for (int i = 0; i < 5; i++) {
        Error err = BURROW_NO_ERROR;
        ConstraintExpr x = constraint_parse(a, lines[i], &err);
        if (!BURROW_OK(err)) {
            fmt_printf_v("%q: %s\n", lines[i], error_text(err));
            continue;
        }
        fmt_printf_v("%s  build=%v min=%q\n", constraint_expr_string(x, a),
                     constraint_expr_eval(x, ok), constraint_go_version(a, x));
    }
    // doc: end
}

static void plus_build(Alloc *a) {
    // doc: plusbuild
    Error err = BURROW_NO_ERROR;
    ConstraintExpr x =
        constraint_parse(a, BURROW_S("//go:build !(windows || plan9) && cgo"), &err);
    Slice lines = slice_nil(TYPE_STRING);
    if (BURROW_OK(err))
        lines = constraint_plus_build_lines(a, x, &err);
    if (!BURROW_OK(err))
        fmt_println_v(error_text(err));
    for (Int i = 0; i < lines.len; i++)
        fmt_println_v(((Str *)lines.p)[i]);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    parse(arena_allocator(&ar));
    plus_build(arena_allocator(&ar));
    arena_free(&ar);
    return 0;
}

/* Output:
linux && (amd64 || arm64)  build=true min=""
!windows && go1.21  build=true min="go1.21"
(darwin && !cgo) || freebsd  build=false min=""
"//go:build linux &&": unexpected end of expression
"// just a comment": not a build constraint
// +build !windows,!plan9,cgo
*/

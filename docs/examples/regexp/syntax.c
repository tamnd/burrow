#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/regexp/syntax.h"

static void show(const char *label, Str s) {
    printf("%s: %.*s\n", label, (int)s.len, (const char *)s.p);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: parse
    Error err;
    SyntaxRegexp *re =
        syntax_parse(a, BURROW_S("(?i)ab{2,3}(?P<tail>[x-z]+)"), SYNTAX_PERL, &err);
    show("String", syntax_regexp_string(re, a));
    printf("top op: %.*s, %d subs\n", (int)syntax_op_string(re->op).len,
           (const char *)syntax_op_string(re->op).p, (int)re->sub.len);
    printf("captures: %d\n", (int)syntax_regexp_max_cap(re));
    Slice names = syntax_regexp_cap_names(re, a); /* "", "tail" */
    show("name 1", ((Str *)names.p)[1]);
    // doc: end

    // doc: simplify
    SyntaxRegexp *s = syntax_regexp_simplify(re, a);
    show("Simplify", syntax_regexp_string(s, a));
    // doc: end

    // doc: compile
    SyntaxRegexp *lit =
        syntax_parse(a, BURROW_S("hello, (world|there)"), SYNTAX_PERL, &err);
    SyntaxProg *prog = syntax_compile(a, lit, &err);
    bool complete;
    Str prefix = syntax_prog_prefix(prog, a, &complete); /* "hello, " */
    printf("prefix \"%.*s\", complete %d, %d instructions\n", (int)prefix.len,
           (const char *)prefix.p, complete, (int)prog->inst.len);
    // doc: end

    // doc: errors
    SyntaxRegexp *bad = syntax_parse(a, BURROW_S("a(b"), SYNTAX_PERL, &err);
    if (bad == NULL) {
        show("error", error_text(err));
        const SyntaxError *se = (const SyntaxError *)errors_as(err, TYPE_SYNTAX_ERROR);
        show("code", se->code);
        show("expr", se->expr);
    }
    // doc: end

    // doc: prog
    SyntaxRegexp *small = syntax_parse(a, BURROW_S("a+b"), SYNTAX_PERL, &err);
    SyntaxProg *p = syntax_compile(a, small, &err);
    Str dump = syntax_prog_string(p, a);
    printf("%.*s", (int)dump.len, (const char *)dump.p);
    // doc: end

    syntax_prog_free(p);
    syntax_regexp_free(small);
    syntax_prog_free(prog);
    syntax_regexp_free(lit);
    syntax_regexp_free(s);
    syntax_regexp_free(re);
    arena_free(&ar);
    return 0;
}

/* Output:
String: (?i:AB{2,3}(?P<tail>[X-Zx-z]+))
top op: Concat, 3 subs
captures: 1
name 1: tail
Simplify: (?i:ABBB?(?P<tail>[X-Zx-z]+))
prefix "hello, ", complete 0, 22 instructions
error: error parsing regexp: missing closing ): `a(b`
code: missing closing )
expr: a(b
  0	fail
  1*	rune1 "a" -> 2
  2	alt -> 1, 3
  3	rune1 "b" -> 4
  4	match
*/

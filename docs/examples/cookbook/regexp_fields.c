// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Str text = S("order 1041 shipped to ada@example.com on 2026-03-08\n"
                 "order 1042 shipped to bob@example.org on 2026-03-09\n"
                 "order 1043 is waiting\n");

    /* Compile once and use it many times. must_compile is for a pattern that
     * is part of the program, where a mistake is a bug, not an input error.
     * In a C string each backslash in the pattern is written twice. */
    Regexp *re = regexp_must_compile(
        a, S("order (?P<id>\\d+) shipped to (?P<email>\\S+) on (?P<date>[\\d-]+)"));
    Int id = regexp_subexp_index(re, S("id"));
    Int email = regexp_subexp_index(re, S("email"));
    Int date = regexp_subexp_index(re, S("date"));

    /* Each match is a Slice of Str: the whole match first, then each group. */
    Slice matches = regexp_find_all_string_submatch(re, a, text, -1);
    for (Int i = 0; i < matches.len; i++) {
        Slice m = AT(Slice, matches, i);
        Str sid = AT(Str, m, id), semail = AT(Str, m, email), sdate = AT(Str, m, date);
        fmt_printf_v("%s  %s  %s\n", sid, sdate, semail);
    }

    /* The same groups can rewrite the text instead. */
    Regexp *at = regexp_must_compile(a, S("(\\w+)@(\\w+)\\.\\w+"));
    Str hidden = regexp_replace_all_string(at, a, text, S("$1 at $2"));
    fmt_print_v(hidden);

    /* The arena holds the patterns, but the memory a search works in comes
     * from the heap, and regexp_free gives it back. */
    regexp_free(at);
    regexp_free(re);
    arena_free(&ar);
}
// doc: end

/* Output:
1041  2026-03-08  ada@example.com
1042  2026-03-09  bob@example.org
order 1041 shipped to ada at example on 2026-03-08
order 1042 shipped to bob at example on 2026-03-09
order 1043 is waiting
*/

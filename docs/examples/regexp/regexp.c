#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

static void show(const char *label, Str s) {
    printf("%s: %.*s\n", label, (int)s.len, (const char *)s.p);
}

static void show_strs(const char *label, Slice v) {
    printf("%s:", label);
    for (Int i = 0; i < v.len; i++) {
        Str s = ((Str *)v.p)[i];
        printf(" [%.*s]", (int)s.len, (const char *)s.p);
    }
    printf("\n");
}

static Str shout(void *env, Str m) {
    return strings_to_upper((Alloc *)env, m);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: compile
    Error err;
    Regexp *re = regexp_compile(a, BURROW_S("(\\w+)@(\\w+)\\.com"), &err);
    if (re == NULL) {
        show("error", error_text(err));
        return 1;
    }
    Str text = BURROW_S("mail ana@example.com or bo@test.com");
    printf("matches: %d\n", regexp_match_string(re, text));
    show("first", regexp_find_string(re, text));
    // doc: end

    // doc: submatch
    Slice m = regexp_find_string_submatch(re, a, text);
    show_strs("groups", m);
    Slice idx = regexp_find_string_submatch_index(re, a, text);
    printf("user at %d to %d\n", (int)((Int *)idx.p)[2], (int)((Int *)idx.p)[3]);
    // doc: end

    // doc: all
    Slice all = regexp_find_all_string(re, a, text, -1);
    show_strs("all", all);
    Regexp *comma = regexp_must_compile(a, BURROW_S(" *, *"));
    Slice words = regexp_split(comma, a, BURROW_S("a, b ,c,  d"), -1);
    show_strs("split", words);
    regexp_free(comma);
    // doc: end

    // doc: replace
    Regexp *named = regexp_must_compile(a, BURROW_S("(?P<user>\\w+)@(?P<host>\\w+)"));
    show("swapped",
         regexp_replace_all_string(named, a, text, BURROW_S("${host} at $user")));
    show("literal",
         regexp_replace_all_literal_string(named, a, text, BURROW_S("$user")));
    show("func",
         regexp_replace_all_string_func(named, a, text, BURROW_FN(StrFunc, shout, a)));
    regexp_free(named);
    // doc: end

    // doc: errors
    Regexp *bad = regexp_compile(a, BURROW_S("a(b"), &err);
    if (bad == NULL)
        show("error", error_text(err));
    // doc: end

    // Searches keep working memory on the heap, so free a Regexp even when
    // its allocator is an arena.
    regexp_free(re);
    arena_free(&ar);
    return 0;
}

/* Output:
matches: 1
first: ana@example.com
groups: [ana@example.com] [ana] [example]
user at 5 to 8
all: [ana@example.com] [bo@test.com]
split: [a] [b] [c] [d]
swapped: mail example at ana.com or test at bo.com
literal: mail $user.com or $user.com
func: mail ANA@EXAMPLE.com or BO@TEST.com
error: error parsing regexp: missing closing ): `a(b`
*/

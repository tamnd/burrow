#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

static void integers(Alloc *a) {
    // doc: parse
    Error err;
    int64_t n = strconv_parse_int(BURROW_S("-0x_7f"), 0, 8, &err);
    // doc: end
    printf("%lld %d\n", (long long)n, BURROW_OK(err));

    // doc: range
    int64_t big = strconv_parse_int(BURROW_S("300"), 10, 8, &err);
    if (errors_is(err, strconv_err_range))
        printf("clamped to %lld: " BURROW_STR_FMT "\n", (long long)big,
               BURROW_STR_ARG(error_text(err)));
    // doc: end

    // doc: numerror
    (void)strconv_atoi(BURROW_S("12a"), &err);
    const StrconvNumError *ne = errors_as(err, TYPE_STRCONV_NUM_ERROR);
    if (ne != NULL)
        printf(BURROW_STR_FMT " failed on " BURROW_STR_FMT "\n",
               BURROW_STR_ARG(ne->func), BURROW_STR_ARG(ne->num));
    // doc: end

    // doc: format
    Str hex = strconv_format_int(a, -255, 16);
    Str dec = strconv_itoa(a, 1234567);
    Slice line = strconv_append_uint(a, slice_from_str(a, BURROW_S("id=")), 42, 10);
    // doc: end
    printf(BURROW_STR_FMT " " BURROW_STR_FMT " %.*s\n", BURROW_STR_ARG(hex),
           BURROW_STR_ARG(dec), (int)line.len, (const char *)line.p);
}

static void booleans(void) {
    // doc: bool
    bool on = strconv_parse_bool(BURROW_S("True"), NULL);
    Str text = strconv_format_bool(on);
    // doc: end
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(text));
}

static void quoting(Alloc *a) {
    // doc: quote
    Str q = strconv_quote(a, BURROW_S("tab\there, bell\a, ☺"));
    // doc: end
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(q));

    // doc: variants
    Str ascii = strconv_quote_to_ascii(a, BURROW_S("naïve ☺"));
    Str rune = strconv_quote_rune(a, 0x263a);
    Str bad = strconv_quote(a, BURROW_S("\xff\xfe"));
    // doc: end
    printf(BURROW_STR_FMT " " BURROW_STR_FMT " " BURROW_STR_FMT "\n",
           BURROW_STR_ARG(ascii), BURROW_STR_ARG(rune), BURROW_STR_ARG(bad));
}

static void appending(Alloc *a) {
    // doc: append
    Slice line = slice_from_str(a, BURROW_S("key="));
    line = strconv_append_quote(a, line, BURROW_S("a \"quoted\" value"));
    // doc: end
    printf("%.*s\n", (int)line.len, (const char *)line.p);
}

static void unquoting(Alloc *a) {
    // doc: unquote
    Error err;
    Str s = strconv_unquote(a, BURROW_S("\"caf\\u00e9\\n\""), &err);
    if (BURROW_FAILED(err))
        printf("not a literal: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
    // doc: end
    printf("%lld bytes, ends in newline %d\n", (long long)s.len,
           s.p[s.len - 1] == '\n');

    // doc: syntax
    (void)strconv_unquote(a, BURROW_S("\"unterminated"), &err);
    if (errors_is(err, strconv_err_syntax))
        printf("bad literal\n");
    // doc: end

    // doc: prefix
    Str rest = BURROW_S("\"first\" and then the rest");
    Str lit = strconv_quoted_prefix(rest, NULL);
    // doc: end
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(lit));

    // doc: borrow
    Str raw = BURROW_S("`C:\\no\\escapes`");
    Str inside = strconv_unquote(a, raw, NULL); /* points into raw */
    // doc: end
    printf(BURROW_STR_FMT " borrowed %d\n", BURROW_STR_ARG(inside),
           inside.p == raw.p + 1);
}

static void printable(void) {
    // doc: print
    bool p = strconv_is_print(0x00a0);   /* no-break space: false */
    bool g = strconv_is_graphic(0x00a0); /* true */
    // doc: end
    printf("print %d, graphic %d, backquote %d\n", p, g,
           strconv_can_backquote(BURROW_S("C:\\temp")));
}

int main(void) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    integers(a);
    booleans();
    quoting(a);
    appending(a);
    unquoting(a);
    printable();

    arena_free(&ar);
    return 0;
}

/* Output:
-127 1
clamped to 127: strconv.ParseInt: parsing "300": value out of range
Atoi failed on 12a
-ff 1234567 id=42
true
"tab\there, bell\a, ☺"
"na\u00efve \u263a" '☺' "\xff\xfe"
key="a \"quoted\" value"
6 bytes, ends in newline 1
bad literal
"first"
C:\no\escapes borrowed 1
print 0, graphic 1, backquote 1
*/

#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"

static void search(void) {
    // doc: search
    Str line = BURROW_S("  GET /index.html HTTP/1.1  ");
    Str req = strings_trim_space(line);
    bool get = strings_has_prefix(req, BURROW_S("GET "));
    Int slash = strings_index_byte(req, '/');
    Int html = strings_count(req, BURROW_S(".html"));
    // doc: end
    printf("%d %lld %lld\n", get, (long long)slash, (long long)html);
}

static void cut(void) {
    // doc: cut
    Str host, port;
    bool found;
    host = strings_cut(BURROW_S("example.com:8080"), BURROW_S(":"), &port, &found);
    // doc: end
    printf(BURROW_STR_FMT " " BURROW_STR_FMT " %d\n", BURROW_STR_ARG(host),
           BURROW_STR_ARG(port), found);
}

static void split(Alloc *a) {
    // doc: split
    Slice parts = strings_split(a, BURROW_S("a,b,,c"), BURROW_S(","));
    for (Int i = 0; i < parts.len; i++) {
        Str p = BURROW_AT(Str, parts, i);
        printf("[" BURROW_STR_FMT "]", BURROW_STR_ARG(p));
    }
    printf("\n");
    // doc: end

    // doc: fields
    Slice words = strings_fields(a, BURROW_S("  the quick\tbrown\n fox "));
    Str joined = strings_join(a, words, BURROW_S("-"));
    // doc: end
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(joined));
}

// doc: yield
static bool print_line(void *env, const void *v) {
    Int *n = (Int *)env;
    Str line = *(const Str *)v;
    printf("%lld: " BURROW_STR_FMT, (long long)++*n, BURROW_STR_ARG(line));
    return true;
}
// doc: end

static void lines(Alloc *a) {
    // doc: lines
    Int n = 0;
    IterSeq seq = strings_lines(a, BURROW_S("one\ntwo\nthree\n"));
    BURROW_CALLF(seq, BURROW_FN(IterYield, print_line, &n));
    // doc: end
}

static void change(Alloc *a) {
    // doc: case
    Str upper = strings_to_upper(a, BURROW_S("gopher"));
    Str same = strings_to_upper(a, upper);
    bool fold = strings_equal_fold(BURROW_S("Straße"), BURROW_S("STRASSE"));
    bool sigma = strings_equal_fold(BURROW_S("\xcf\x83"), BURROW_S("\xce\xa3"));
    // doc: end
    printf(BURROW_STR_FMT " %d %d %d\n", BURROW_STR_ARG(upper), same.p == upper.p, fold,
           sigma);

    // doc: replace
    Str s = strings_replace_all(a, BURROW_S("oink oink oink"), BURROW_S("k"),
                                BURROW_S("ky"));
    Str t = strings_replace(a, s, BURROW_S("oinky"), BURROW_S("moo"), 2);
    // doc: end
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(t));
}

static void builder(Alloc *a) {
    // doc: builder
    StringsBuilder b = STRINGS_BUILDER(a);
    for (int i = 3; i > 0; i--) {
        strings_builder_write_string(&b, BURROW_S("tick "), NULL);
        strings_builder_write_byte(&b, (Byte)('0' + i));
        strings_builder_write_byte(&b, '\n');
    }
    strings_builder_write_rune(&b, 0x1F680, NULL);
    Str out = strings_builder_string(&b);
    // doc: end
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(out));
}

static void replacer(Alloc *a) {
    // doc: replacer
    static const Str pairs[] = {
        BURROW_S_INIT("<"),    BURROW_S_INIT("&lt;"), BURROW_S_INIT(">"),
        BURROW_S_INIT("&gt;"), BURROW_S_INIT("&"),    BURROW_S_INIT("&amp;"),
    };
    Slice list = slice_from((void *)(uintptr_t)pairs, 6, 6, TYPE_STRING);
    StringsReplacer *r = strings_new_replacer(a, list);
    Str safe = strings_replacer_replace(r, a, BURROW_S("a < b && c > d"));
    // doc: end
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(safe));
}

static void reader(Alloc *a) {
    // doc: reader
    StringsReader *r = strings_new_reader(a, BURROW_S("h\xc3\xa9llo"));
    Error err = BURROW_NO_ERROR;
    for (;;) {
        Int size;
        Rune c = strings_reader_read_rune(r, &size, &err);
        if (BURROW_FAILED(err))
            break;
        printf("U+%04X %lld\n", (unsigned)c, (long long)size);
    }
    // doc: end
    printf("%d\n", errors_is(err, io_eof));
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    search();
    cut();
    split(a);
    lines(a);
    change(a);
    builder(a);
    replacer(a);
    reader(a);
    arena_free(&ar);
    return 0;
}

/* Output:
1 4 1
example.com 8080 1
[a][b][][c]
the-quick-brown-fox
1: one
2: two
3: three
GOPHER 1 0 1
moo moo oinky
tick 3
tick 2
tick 1
🚀
a &lt; b &amp;&amp; c &gt; d
U+0068 1
U+00E9 2
U+006C 1
U+006C 1
U+006F 1
1
*/

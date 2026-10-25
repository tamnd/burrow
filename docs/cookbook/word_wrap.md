# Wrap text to a width

`strings_fields` splits text into words on any run of white space, which throws the old line breaks away. Adding words to a line until the next one won't fit gives the new lines. Counting runes with `utf8_rune_count_in_string` rather than bytes keeps text with accented letters the right width.

<!-- example: ../examples/cookbook/word_wrap.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

/* Wraps text into lines of at most width characters, breaking only between
 * words. Each line starts with prefix, which counts toward the width. A word
 * longer than the width gets a line to itself. Widths count runes, not
 * bytes, so text that isn't ASCII lines up too. */
static Str wrap(Alloc *a, Str text, Int width, Str prefix) {
    Slice words = strings_fields(a, text);
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err = NO_ERROR;
    Int start = utf8_rune_count_in_string(prefix), col = 0;
    bool empty = true; /* nothing on the current line yet */
    for (Int i = 0; i < words.len; i++) {
        Str w = AT(Str, words, i);
        Int n = utf8_rune_count_in_string(w);
        if (!empty && col + 1 + n > width) {
            strings_builder_write_byte(&b, '\n');
            empty = true;
        }
        if (empty) {
            strings_builder_write_string(&b, prefix, &err);
            col = start;
        } else {
            strings_builder_write_byte(&b, ' ');
            col++;
        }
        strings_builder_write_string(&b, w, &err);
        col += n;
        empty = false;
    }
    return strings_builder_string(&b);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* strings_fields splits on any run of spaces, tabs and line breaks, so
     * the old line breaks go away and the text flows into the new width. */
    Str text = S("The quick brown fox jumps over the lazy dog.\n"
                 "Le cœur a ses raisons que la raison ne connaît point.");
    fmt_println_v(wrap(a, text, 24, S("")));
    fmt_println_v("---");
    fmt_println_v(wrap(a, text, 30, S("> ")));
    arena_free(&ar);
}
```

It prints:

```text
The quick brown fox
jumps over the lazy dog.
Le cœur a ses raisons
que la raison ne connaît
point.
---
> The quick brown fox jumps
> over the lazy dog. Le cœur a
> ses raisons que la raison ne
> connaît point.
```

The program is [docs/examples/cookbook/word_wrap.c](../examples/cookbook/word_wrap.c). CI builds and runs it and checks that it prints this.

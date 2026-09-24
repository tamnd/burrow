# Character classes and case

`burrow/unicode.h` is Go's `unicode` package. It answers questions about a single rune: is it a letter, a digit, a space, which script is it from, and what are its other cases. [Runes and UTF-8](runes.md) covers getting runes out of bytes in the first place. This guide picks up once you have one.

The answers come from the same tables Go uses, generated from Go's own `tables.go`, so a rune that Go calls a letter is a letter here too. `UNICODE_VERSION` says which Unicode release they come from, 17.0.0 at the moment.

## Asking what a rune is

The predicates take a rune and return a bool. This is the usual shape of a lexer's inner loop:

<!-- example: ../examples/unicode/unicode.c#classify -->
```c
Str s = BURROW_S("Go 1.27, \xce\xb1\xce\xb2!");
Int i;
Rune r;
for (StrIter it = str_runes(s); str_next_rune(&it, &i, &r);) {
    printf("%-7s", unicode_is_letter(r)  ? "letter"
                   : unicode_is_digit(r) ? "digit"
                   : unicode_is_space(r) ? "space"
                   : unicode_is_punct(r) ? "punct"
                                         : "other");
    printf(" U+%04X%s\n", (unsigned)r, unicode_is_upper(r) ? " upper" : "");
}
```

The Greek alpha and beta come out as letters, the same as `G` and `o`. The full set is `unicode_is_letter`, `unicode_is_digit`, `unicode_is_number`, `unicode_is_space`, `unicode_is_punct`, `unicode_is_symbol`, `unicode_is_mark`, `unicode_is_control`, `unicode_is_upper`, `unicode_is_lower`, `unicode_is_title`, `unicode_is_graphic` and `unicode_is_print`, one for each of Go's `IsX` functions.

Two of them are easy to mix up. `unicode_is_digit` is the decimal digits, category Nd, which includes the Arabic-Indic and Devanagari digits but not Roman numerals. `unicode_is_number` is all of category N, so it takes Ⅻ and ½ as well. For plain ASCII parsing, compare with `'0'` and `'9'` yourself. A config file that accepts a Thai digit as a port number is rarely what anyone meant.

`unicode_is_space` is not the same as category Z either. It follows the White_Space property, so it includes tab, newline and carriage return, which are control characters rather than separators.

All of these are fast for ASCII and Latin-1, where they read one byte from a 256 entry table, and fall back to a search of the range tables above that. A rune that is not valid at all, negative or above `UNICODE_MAX_RUNE`, gives false from every one of them.

## Changing case

`unicode_to_upper`, `unicode_to_lower` and `unicode_to_title` map one rune to one rune:

<!-- example: ../examples/unicode/unicode.c#case -->
```c
Rune dz = 0x01C6; // the digraph dz with caron, as one lower case letter
Rune upper = unicode_to_upper(dz);
Rune title = unicode_to_title(dz);
Rune lower = unicode_to_lower(upper);
```

Title case is usually the same as upper case, and this is one of the few characters where it is not: the upper case is `01C4`, all capitals, and the title case is `01C5`, a capital D with a small z, for the start of a word. `unicode_to(UNICODE_TITLE_CASE, r)` is the same call with the case as an argument.

These are simple case mappings, one rune in and one rune out, and that is all Go's `unicode` package does. The German ß upper cases to itself here rather than to SS, because SS is two runes. A rune with no other case maps to itself.

Some languages need different mappings. Turkish and Azeri have a dotted and a dotless i, and upper casing a plain `i` has to give `İ`, U+0130:

<!-- example: ../examples/unicode/unicode.c#special -->
```c
Rune i_upper = unicode_to_upper('i');
Rune tr_upper = unicode_special_case_to_upper(unicode_turkish_case, 'i');
```

The first is `0049`, an ordinary `I`, and the second is `0130`. A `UnicodeSpecialCase` is a sorted list of ranges, and anything it does not cover falls through to the default mappings, so you can make your own for a language burrow does not ship.

## Comparing without case

`unicode_simple_fold` gives the next rune in the set of runes that are equal ignoring case. Following it round from any member visits all of them:

<!-- example: ../examples/unicode/unicode.c#fold -->
```c
printf("U+%04X", (unsigned)'k');
for (Rune r = unicode_simple_fold('k'); r != 'k'; r = unicode_simple_fold(r))
    printf(" U+%04X", (unsigned)r);
printf("\n");
```

That prints `U+006B U+212A U+004B`. The middle one is the Kelvin sign, which folds to `k` like the capital letter does. This is why comparing `unicode_to_lower` of both sides is not the same as a case insensitive comparison, and why Go's `strings.EqualFold` walks these orbits instead. Most sets have two members, some have three or four, and a rune with no case gives itself back.

## Tables

Every table Go exports is here under its name in snake case: `unicode_greek`, `unicode_han`, `unicode_lu`, `unicode_white_space`, and so on, 262 of them. Each one is a pointer to a constant `UnicodeRangeTable`, so there is nothing to set up and nothing to free. `unicode_is` tests one, and `unicode_in_v` tests a list:

<!-- example: ../examples/unicode/unicode.c#tables -->
```c
Rune alpha = 0x03B1;
bool greek = unicode_is(unicode_greek, alpha);
bool either = unicode_in_v(alpha, unicode_latin, unicode_cyrillic);
```

`unicode_in` is the same test with the tables in a `Slice`, which is how `UNICODE_RANGES` builds them and how `unicode_graphic_ranges` and `unicode_print_ranges` are kept. Go's `IsOneOf` is `unicode_is_one_of`, with the arguments the other way round.

Go also keeps maps from a name to a table: `Categories`, `Scripts`, `Properties`, `FoldCategory` and `FoldScript`. Here each one is a `UnicodeTableMap`, which is an array sorted by name, and `unicode_table_map_get` finds a name in it:

<!-- example: ../examples/unicode/unicode.c#lookup -->
```c
const UnicodeRangeTable *t =
    unicode_table_map_get(unicode_scripts, BURROW_S("Hiragana"));
if (t != NULL && unicode_is(t, 0x3042))
    printf("U+3042 is Hiragana\n");
```

The lookup is how a regular expression engine turns `\p{Hiragana}` into a table. A name that is not in the map gives `NULL`, the same as a missing key in Go gives nil.

Because the entries are an array, walking one is a plain loop. This finds which two letter category a rune is in:

<!-- example: ../examples/unicode/unicode.c#walk -->
```c
for (Int k = 0; k < unicode_categories.len; k++) {
    UnicodeNamedTable e = unicode_categories.p[k];
    if (e.name.len == 2 && unicode_is(e.table, 0x20AC))
        printf("U+20AC is in " BURROW_STR_FMT "\n", BURROW_STR_ARG(e.name));
}
```

The euro sign is in Sc, currency symbols. Go's map has no order, so a Go loop over it visits the names in a different order each run. This one goes alphabetically every time.

`unicode_category_aliases` maps the long names of the categories, such as `Letter` and `Currency_Symbol`, to the short ones. `unicode_alias_map_get` looks a name up in it.

## How the tables are made

`tools/gen-unicode/main.go` reads Go's `tables.go` and `casetables.go` with `go/types` and writes `src/unicode/tables.c`, along with the declarations in `unicode.h`. It reads Go's tables rather than the Unicode Character Database because matching Go is the point. Go's tables are themselves generated from the UCD, and if the two were ever built from different releases, or Go made a choice about a property that a fresh reading of the UCD would make differently, burrow would still agree with Go. When Go moves to a new Unicode release, running the generator against the new Go picks it up.

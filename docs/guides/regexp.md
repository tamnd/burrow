# Regular expressions

Go's regular expressions are RE2's: they run in time linear in the input, and to make that possible they leave out backreferences and lookaround. burrow has the same syntax and the same guarantee. `burrow/regexp.h` is Go's `regexp`, the package most programs want, and `burrow/regexp/syntax.h` is Go's `regexp/syntax`, the parser and compiler underneath it, covered further down.

The names are Go's with the package in front: `regexp.Compile` is `regexp_compile`, `Regexp.FindAllString` is `regexp_find_all_string` and `syntax.Parse` is `syntax_parse`.

## The syntax

A single character matches itself, except for the metacharacters `\.+*?()|[]{}^$`, which a backslash turns back into plain characters. The rest, in short:

| Pattern | Matches |
| --- | --- |
| `.` | any character, and newline too with the `s` flag |
| `[xyz]`, `[^xyz]` | a character class, or its complement |
| `\d`, `\w`, `\s` | Perl classes: ASCII digits, word characters and spaces, and `\D`, `\W`, `\S` for the complements |
| `[[:alpha:]]` | an ASCII class by name, and `[[:^alpha:]]` for the complement |
| `\pN`, `\p{Greek}` | a Unicode category or script, and `\PN`, `\P{Greek}` for the complement |
| `xy`, `x\|y` | x then y, x or y |
| `x*`, `x+`, `x?` | zero or more, one or more, zero or one, and `x*?` and the rest for the non-greedy forms |
| `x{n,m}`, `x{n,}`, `x{n}` | between n and m, n or more, exactly n, where n and m are at most 1000 |
| `(re)`, `(?P<name>re)`, `(?<name>re)` | a numbered capture, and a named one |
| `(?:re)` | a group that does not capture |
| `(?flags)`, `(?flags:re)` | set flags for the rest of the group, or just for re |
| `^`, `$` | start and end of text, or of a line with the `m` flag |
| `\A`, `\z` | start and end of text, always |
| `\b`, `\B` | an ASCII word boundary, and not one |

The flags are `i` for case-insensitive, `m` for multi-line `^` and `$`, `s` for letting `.` match a newline and `U` for swapping greedy and non-greedy. `(?i)abc` and `(?i:abc)` both work, and `(?i-s)` sets one flag and clears another.

## Package regexp

`regexp_compile` parses a pattern and gets it ready to match. The Regexp it gives can be used from any number of threads at once, and `regexp_free` gives it back. `regexp_must_compile` panics on a bad pattern instead, for patterns written into the program. The Match functions say whether there is a match anywhere in the text, and `regexp_find_string` gives the leftmost one:

<!-- example: ../examples/regexp/regexp.c#compile -->
```c
Error err;
Regexp *re = regexp_compile(a, BURROW_S("(\\w+)@(\\w+)\\.com"), &err);
if (re == NULL) {
    show("error", error_text(err));
    return 1;
}
Str text = BURROW_S("mail ana@example.com or bo@test.com");
printf("matches: %d\n", regexp_match_string(re, text));
show("first", regexp_find_string(re, text));
```

Each function comes in the forms Go has: one for a `Slice` of bytes, one for a `Str` with `String` in the name and, for the few that make sense, one for an `IoRuneReader` with `Reader` in the name. What a Find function returns points into the text it was given, and the slices that hold the results come from the allocator passed in. An arena is the easy way to deal with them.

### Groups

The Submatch forms give the text of each group as well, with the whole match first, and the Index forms give where things are as pairs of start and end offsets. A group that took no part in the match is the empty string, or -1 in the Index forms:

<!-- example: ../examples/regexp/regexp.c#submatch -->
```c
Slice m = regexp_find_string_submatch(re, a, text);
show_strs("groups", m);
Slice idx = regexp_find_string_submatch_index(re, a, text);
printf("user at %d to %d\n", (int)((Int *)idx.p)[2], (int)((Int *)idx.p)[3]);
```

`regexp_num_subexp`, `regexp_subexp_names` and `regexp_subexp_index` say how many groups there are and what they are called.

### Every match

The FindAll forms give every match that does not overlap the one before, and take a limit, where -1 means all of them. `regexp_split` cuts the text around the matches:

<!-- example: ../examples/regexp/regexp.c#all -->
```c
Slice all = regexp_find_all_string(re, a, text, -1);
show_strs("all", all);
Regexp *comma = regexp_must_compile(a, BURROW_S(" *, *"));
Slice words = regexp_split(comma, a, BURROW_S("a, b ,c,  d"), -1);
show_strs("split", words);
regexp_free(comma);
```

### Replacing

`regexp_replace_all_string` replaces each match with a template where `$1` or `${1}` is a group by number and `$name` or `${name}` a group by name. A name runs as far as the letters, digits and underscores go, so `$1x` is the group called `1x`; write `${1}x` for group 1 then an x. `$$` is a dollar sign. The Literal forms take the replacement as it is, and the Func forms call a function with each match, a `StrFunc` or a `BytesFunc`, and copy what it returns into the result:

<!-- example: ../examples/regexp/regexp.c#replace -->
```c
Regexp *named = regexp_must_compile(a, BURROW_S("(?P<user>\\w+)@(?P<host>\\w+)"));
show("swapped",
     regexp_replace_all_string(named, a, text, BURROW_S("${host} at $user")));
show("literal",
     regexp_replace_all_literal_string(named, a, text, BURROW_S("$user")));
show("func",
     regexp_replace_all_string_func(named, a, text, BURROW_FN(StrFunc, shout, a)));
regexp_free(named);
```

`regexp_expand_string` fills in a template the same way from one match, for when the result goes somewhere other than back into the text.

### Leftmost first and leftmost longest

`regexp_compile` gives the match Perl and Python would, where the first alternative that works wins, so `a(|b)` on `ab` matches `a`. `regexp_compile_posix` takes egrep syntax and gives the leftmost longest match, `ab`, the POSIX rule. `regexp_longest` switches a Regexp already compiled over to that rule.

### How matching works

There are three matchers, the same three Go has, and each Regexp picks one per search. A pattern anchored at the start where every step is decided by the next character runs on the one pass matcher, which never goes back. A small pattern on short input runs on a backtracker that remembers where it has been, so it still takes linear time. Anything else runs on a Pike VM that keeps all the alternatives going at once. They all give the same answers, and the tests hold each one to Go's output on the same cases. What a search needs to work with is kept in the Regexp and reused, so a search on a Regexp that has been used before allocates nothing but its results.

### Errors and memory

A bad pattern gives a `SyntaxError` with Go's message, and `errors_as` with `TYPE_SYNTAX_ERROR` gets the code and the text back:

<!-- example: ../examples/regexp/regexp.c#errors -->
```c
Regexp *bad = regexp_compile(a, BURROW_S("a(b"), &err);
if (bad == NULL)
    show("error", error_text(err));
```

A Regexp and everything it keeps come from the allocator given to `regexp_compile`, except the working memory of searches, which comes from the heap. When an allocator runs out, compiling gives `burrow_err_out_of_memory` and the functions that return a slice or a string give the nil slice or the empty string, with nothing left allocated. `regexp_quote_meta` gives back the string it was passed when there is nothing in it to quote, so check before freeing its result.

## Package regexp/syntax

Most programs never need this package. It is there for tools that want the parsed tree or the compiled program.

### Parsing

`syntax_parse` takes the pattern and a set of `SyntaxFlags` and gives a tree. `SYNTAX_PERL` is what package `regexp` uses for `regexp.Compile`, and `SYNTAX_POSIX` is the stricter egrep syntax behind `regexp.CompilePOSIX`. The tree prints back as a pattern that means the same thing:

<!-- example: ../examples/regexp/syntax.c#parse -->
```c
Error err;
SyntaxRegexp *re =
    syntax_parse(a, BURROW_S("(?i)ab{2,3}(?P<tail>[x-z]+)"), SYNTAX_PERL, &err);
show("String", syntax_regexp_string(re, a));
printf("top op: %.*s, %d subs\n", (int)syntax_op_string(re->op).len,
       (const char *)syntax_op_string(re->op).p, (int)re->sub.len);
printf("captures: %d\n", (int)syntax_regexp_max_cap(re));
Slice names = syntax_regexp_cap_names(re, a); /* "", "tail" */
show("name 1", ((Str *)names.p)[1]);
```

Each node has an `op`, its children in `sub`, the characters it matches in `rune` and, for a repeat, `min` and `max`. Character classes keep their ranges in `rune` as pairs of low and high, sorted and merged, so `[x-z]` under `(?i)` becomes the two pairs `X-Z` and `x-z`. The name of each capture is in `name`, and `syntax_regexp_cap_names` lists them all by number, with the empty string for the whole match and for the captures that have no name.

### Simplifying and compiling

`syntax_regexp_simplify` rewrites counted repetitions like `x{2,3}` into the plain operators, which is what the compiler needs:

<!-- example: ../examples/regexp/syntax.c#simplify -->
```c
SyntaxRegexp *s = syntax_regexp_simplify(re, a);
show("Simplify", syntax_regexp_string(s, a));
```

`syntax_compile` turns a simplified tree into a `SyntaxProg`, the list of instructions the matchers step through. `syntax_prog_prefix` gives the literal text every match has to start with, which lets a matcher skip ahead with a plain string search, and says whether that text is the whole match:

<!-- example: ../examples/regexp/syntax.c#compile -->
```c
SyntaxRegexp *lit =
    syntax_parse(a, BURROW_S("hello, (world|there)"), SYNTAX_PERL, &err);
SyntaxProg *prog = syntax_compile(a, lit, &err);
bool complete;
Str prefix = syntax_prog_prefix(prog, a, &complete); /* "hello, " */
printf("prefix \"%.*s\", complete %d, %d instructions\n", (int)prefix.len,
       (const char *)prefix.p, complete, (int)prog->inst.len);
```

`syntax_prog_string` prints a program the way Go does, one instruction per line, with a star on the one where matching starts:

<!-- example: ../examples/regexp/syntax.c#prog -->
```c
SyntaxRegexp *small = syntax_parse(a, BURROW_S("a+b"), SYNTAX_PERL, &err);
SyntaxProg *p = syntax_compile(a, small, &err);
Str dump = syntax_prog_string(p, a);
printf("%.*s", (int)dump.len, (const char *)dump.p);
```

### Errors

A pattern that does not parse gives a `SyntaxError`, which has the code and the part of the pattern it is about. Its message is Go's, so `a(b` fails with ``error parsing regexp: missing closing ): `a(b` ``. `errors_as` with `TYPE_SYNTAX_ERROR` gets the fields back:

<!-- example: ../examples/regexp/syntax.c#errors -->
```c
SyntaxRegexp *bad = syntax_parse(a, BURROW_S("a(b"), SYNTAX_PERL, &err);
if (bad == NULL) {
    show("error", error_text(err));
    const SyntaxError *se = (const SyntaxError *)errors_as(err, TYPE_SYNTAX_ERROR);
    show("code", se->code);
    show("expr", se->expr);
}
```

The codes are the `SYNTAX_ERR_*` macros, compared with `str_eq`. There are limits on how big a pattern can get, the same ones Go has: 1000 for the count in `{n,m}`, 1000 for how deeply groups can nest, and a cap on the size of the compiled program. A pattern that goes over one of them fails with `SYNTAX_ERR_LARGE` or `SYNTAX_ERR_NESTING_DEPTH`. When the allocator runs out the error is `burrow_err_out_of_memory` and nothing is left allocated.

### Memory

A tree lives in one block from the allocator you pass, and `syntax_regexp_free` gives it back in one go. Nodes can be shared by more than one parent, as in Go, so never free a node on its own. Go's `Simplify` can return the same tree or reuse parts of it, but `syntax_regexp_simplify` always makes a new block, so the two trees can be freed in any order. A program is one block too and keeps its own copy of what it matches, so it can outlive the tree it came from.

The functions that print or list things, `syntax_regexp_string`, `syntax_regexp_cap_names`, `syntax_prog_string` and `syntax_prog_prefix`, allocate their result from the allocator they are given and return the empty value when it runs out.

## See also

- [unicode.md](unicode.md), for the tables behind `\p{...}` and case folding.
- [strings.md](strings.md), for when a plain substring search is enough.

# HTML

`burrow/html.h` is Go's `html` package, which escapes text so it can go into an HTML page and turns character references back into text. `html/template` will land in this guide when it is ported.

## Escaping

`html_escape_string` replaces the five characters that mean something in HTML, `<`, `>`, `&`, `'` and `"`, with character references. Nothing else changes, so the result is safe between tags and inside a quoted attribute value:

<!-- example: ../examples/html/escape.c#escape -->
```c
Str s = BURROW_S("\"Fran & Freddie's Diner\" <tasty@example.com>");
fmt_println_v(html_escape_string(a, s));
```

That prints:

```
&#34;Fran &amp; Freddie&#39;s Diner&#34; &lt;tasty@example.com&gt;
```

When there is nothing to escape you get the same string back, pointing at the same bytes, and nothing is allocated. Otherwise the result is made in the allocator you pass, and comes back empty if the allocator refuses.

## Unescaping

`html_unescape_string` goes the other way, and it knows far more than the five references escaping makes. Every named reference in HTML5 works, as do decimal and hex numeric references:

<!-- example: ../examples/html/escape.c#unescape -->
```c
Str s = BURROW_S("&quot;Fran &amp; Freddie&#39;s Diner&quot; "
                 "&lt;tasty@example.com&gt;");
fmt_println_v(html_unescape_string(a, s));
fmt_println_v(html_unescape_string(a, BURROW_S("caf&eacute; caf&#233; caf&#xE9;")));
fmt_println_v(html_unescape_string(a, BURROW_S("&copy 2026, &notanentity;")));
```

That prints:

```
"Fran & Freddie's Diner" <tasty@example.com>
café café café
© 2026, ¬anentity;
```

The last line shows the rules browsers follow for text that only looks like a reference. A few old names, `&copy` and `&not` among them, are recognised without their semicolon, and the longest such name at the start of a word wins, which is why `&notanentity;` comes out starting with `¬`. Anything that matches nothing is left alone.

Unescaping what escaping made always gives back the text you started with. The reverse is not true, since `&eacute;`, `&#233;` and `&#xE9;` all unescape to the same `é` and escaping leaves that `é` as it is.

#include "burrow/burrow.h"

static void escape(Alloc *a) {
    // doc: escape
    Str s = BURROW_S("\"Fran & Freddie's Diner\" <tasty@example.com>");
    fmt_println_v(html_escape_string(a, s));
    // doc: end
}

static void unescape(Alloc *a) {
    // doc: unescape
    Str s = BURROW_S("&quot;Fran &amp; Freddie&#39;s Diner&quot; "
                     "&lt;tasty@example.com&gt;");
    fmt_println_v(html_unescape_string(a, s));
    fmt_println_v(html_unescape_string(a, BURROW_S("caf&eacute; caf&#233; caf&#xE9;")));
    fmt_println_v(html_unescape_string(a, BURROW_S("&copy 2026, &notanentity;")));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    escape(a);
    unescape(a);
    arena_free(&ar);
    return 0;
}

/* Output:
&#34;Fran &amp; Freddie&#39;s Diner&#34; &lt;tasty@example.com&gt;
"Fran & Freddie's Diner" <tasty@example.com>
café café café
© 2026, ¬anentity;
*/

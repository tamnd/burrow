/* html, escaping and unescaping text for HTML.
 *
 * Go's html package. html_escape_string turns the five characters that mean
 * something in HTML, < > & ' and ", into character references, and
 * html_unescape_string turns references back into the text they stand for:
 *
 *     Str safe = html_escape_string(a, BURROW_S("Fran & Freddie's <Diner>"));
 *     // Fran &amp; Freddie&#39;s &lt;Diner&gt;
 *
 * Unescaping knows every named reference in HTML5, such as &aacute; and
 * &NotEqualTilde;, as well as numeric ones such as &#225; and &#xE1;. Escaping
 * then unescaping always gives back what you started with. The other way round
 * does not, because there are several ways to write the same character.
 *
 * This is for text. To make HTML from a template with escaping that knows
 * where in the page each value lands, use html/template.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package html */

#ifndef BURROW_HTML_H
#define BURROW_HTML_H

#include "burrow/core.h"
#include "burrow/mem.h"

/* EscapeString: s with <, >, &, ' and " replaced by &lt;, &gt;, &amp;, &#39;
 * and &#34;. Returns s itself when it has none of them, and otherwise a new
 * string made in a, or an empty one when a refuses. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str html_escape_string(Alloc *a, Str s);

/* UnescapeString: s with each character reference, such as &lt;, &aacute;,
 * &#225; or &#xE1;, replaced by the character it stands for. It knows more
 * references than html_escape_string makes. Returns s itself when it has no
 * '&', and otherwise a new string made in a, or an empty one when a refuses. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str html_unescape_string(Alloc *a, Str s);

#endif /* BURROW_HTML_H */

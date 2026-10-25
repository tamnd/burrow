// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/encoding/xml.h"

/* The tags say where each field comes from: "title" is a child element of
 * that name, "a>b" reaches down through nested elements, and ",attr" reads
 * an attribute of the element itself. A slice field takes every match. */
#define ITEM_FIELDS(F, T)                                                              \
    F(T, Str, Title, "xml:\"title\"")                                                  \
    F(T, Str, Link, "xml:\"link\"")                                                    \
    F(T, Str, Date, "xml:\"pubDate\"")
BURROW_STRUCT(Item, ITEM_FIELDS);
BURROW_SLICE_TYPE(Items, Item);

#define FEED_FIELDS(F, T)                                                              \
    F(T, Str, Version, "xml:\"version,attr\"")                                         \
    F(T, Str, Title, "xml:\"channel>title\"")                                          \
    F(T, Items, Items, "xml:\"channel>item\"")
BURROW_STRUCT(Feed, FEED_FIELDS);

static const char feed_xml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<rss version=\"2.0\">\n"
    "  <channel>\n"
    "    <title>Burrow news</title>\n"
    "    <link>https://example.com/</link>\n"
    "    <item>\n"
    "      <title>v0.3 is out</title>\n"
    "      <link>https://example.com/v0.3</link>\n"
    "      <pubDate>Mon, 05 Oct 2026 09:00:00 GMT</pubDate>\n"
    "    </item>\n"
    "    <item>\n"
    "      <title>Notes on &lt;stdatomic.h&gt;</title>\n"
    "      <link>https://example.com/atomics</link>\n"
    "      <pubDate>Fri, 16 Oct 2026 14:30:00 GMT</pubDate>\n"
    "    </item>\n"
    "  </channel>\n"
    "</rss>\n";

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Elements with no field to go in, such as the channel's link, are
     * skipped, and entities like &lt; are decoded. */
    Feed feed = {0};
    Int n = (Int)sizeof feed_xml - 1;
    Slice data = slice_from((void *)(uintptr_t)feed_xml, n, n, TYPE_BYTE);
    Error err = xml_unmarshal(a, data, ANY(TYPE_OF(Feed), &feed));
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    fmt_printf_v("%s (RSS %s), %d items\n", feed.Title, feed.Version, feed.Items.len);
    for (Int i = 0; i < feed.Items.len; i++) {
        Item it = AT(Item, feed.Items, i);
        Time t = time_parse(a, TIME_RFC1123, it.Date, &err);
        if (FAILED(err)) {
            fmt_println_v(err);
            return 1;
        }
        fmt_printf_v("%s  %s\n    %s\n", time_format(t, a, S("2006-01-02")), it.Title,
                     it.Link);
    }

    /* A document that isn't well formed gives a syntax error with its line. */
    err = xml_unmarshal(a, BURROW_B("<rss>\n<channel>\n</rss>"),
                        ANY(TYPE_OF(Feed), &feed));
    fmt_println_v(err);
    arena_free(&ar);
}
// doc: end

/* Output:
Burrow news (RSS 2.0), 2 items
2026-10-05  v0.3 is out
    https://example.com/v0.3
2026-10-16  Notes on <stdatomic.h>
    https://example.com/atomics
XML syntax error on line 3: element <channel> closed by </rss>
*/

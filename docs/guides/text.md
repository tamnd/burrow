# Text

`burrow/text/tabwriter.h` is Go's `text/tabwriter`, which lines text up in columns. The rest of Go's `text` tree, `text/scanner` and `text/template`, will land in this guide as they are ported.

## Columns

A tabwriter sits in front of another `IoWriter`. You write cells separated by tabs and lines ended by newlines, and it pads each cell so that every column is as wide as its widest cell:

<!-- example: ../examples/text/tabwriter.c#table -->
```c
TabwriterWriter *w = tabwriter_new_writer(a, out, 0, 8, 2, ' ', 0);
IoWriter tw = tabwriter_writer_as_io_writer(w);
fmt_fprintln_v(tw, "NAME\tSIZE\tKIND");
fmt_fprintf_v(tw, "%s\t%d\t%s\n", "burrow.h", 2048, "header");
fmt_fprintf_v(tw, "%s\t%d\t%s\n", "libburrow.a", 1843200, "archive");
fmt_fprintf_v(tw, "%s\t%d\t%s\n", "README.md", 512, "text");
Error err = tabwriter_writer_flush(w);
tabwriter_writer_free(w);
```

That prints:

```
NAME         SIZE     KIND
burrow.h     2048     header
libburrow.a  1843200  archive
README.md    512      text
```

The numbers after `out` are Go's `minwidth`, `tabwidth` and `padding`, then the pad character and the flags. `minwidth` is the narrowest a column gets, `padding` is added to every cell, and `tabwidth` only matters when the padding is made of tabs or `TABWRITER_TAB_INDENT` is on. `tabwriter_writer_as_io_writer` gives you the writer as an `IoWriter`, which is what `fmt_fprintf` and everything else in the library that writes bytes takes.

Nothing reaches `out` until the writer knows how wide the columns are, which in general is not until the end, so `tabwriter_writer_flush` is not optional. It returns the first error `out` gave, if there was one. A tabwriter from `tabwriter_new_writer` needs `tabwriter_writer_free` as well, unless it came from an arena. You can also zero a `TabwriterWriter` of your own and call `tabwriter_writer_init` on it, which is Go's `new(tabwriter.Writer).Init(...)`. Init can be called again to reuse the writer with different settings, and it keeps the memory the writer has already grown.

## Flags

The flags are Go's with a `TABWRITER_` prefix. `TABWRITER_ALIGN_RIGHT` puts the padding before the text, and `TABWRITER_DEBUG` draws a bar between columns, which is the quickest way to see where the writer thinks your columns are:

<!-- example: ../examples/text/tabwriter.c#right -->
```c
TabwriterWriter *w = tabwriter_new_writer(a, out, 5, 0, 1, ' ',
                                          TABWRITER_ALIGN_RIGHT | TABWRITER_DEBUG);
IoWriter tw = tabwriter_writer_as_io_writer(w);
fmt_fprintln_v(tw, "a\tb\tc\t");
fmt_fprintln_v(tw, "123\t12345\t1234567\t");
tabwriter_writer_flush(w);
tabwriter_writer_free(w);
```

```
    a|     b|       c|
  123| 12345| 1234567|
```

`TABWRITER_FILTER_HTML` treats a tag as zero wide and an entity such as `&amp;` as one character. `TABWRITER_DISCARD_EMPTY_COLUMNS` drops a column in which every cell is empty and ended by a vertical tab. `TABWRITER_TAB_INDENT` pads leading empty cells with tabs whatever the pad character is. Text between two `TABWRITER_ESCAPE` bytes (0xff) is passed through as it is, tabs and newlines included, and `TABWRITER_STRIP_ESCAPE` takes the escape bytes out.

When the pad character is `'\t'` the text is always left aligned, as in Go, since where a tab stops is up to whatever displays it.

## Which cells make a column

This is the part that surprises people. A column is not everything in the same position on every line. It is a run of adjacent lines that all have a cell in that position, and a cell only counts if a tab ends it. The text after the last tab on a line is not in any column:

<!-- example: ../examples/text/tabwriter.c#elastic -->
```c
TabwriterWriter *w = tabwriter_new_writer(a, out, 0, 0, 1, '.', TABWRITER_DEBUG);
IoWriter tw = tabwriter_writer_as_io_writer(w);
fmt_fprintln_v(tw, "a\tb\tc");
fmt_fprintln_v(tw, "aa\tbb\tcc");
fmt_fprintln_v(tw, "aaa\t"); /* no cell in column two, so the b column ends */
fmt_fprintln_v(tw, "aaaa\tdddd\teeee");
tabwriter_writer_flush(w);
tabwriter_writer_free(w);
```

```
a....|b..|c
aa...|bb.|cc
aaa..|
aaaa.|dddd.|eeee
```

The first column runs down all four lines. The second stops at `aaa`, which has nothing after its tab, so `dddd` starts a new column that is sized on its own. In the first example the last column needs no padding, so its lines have no trailing tab. Give every line a trailing tab when the last column should be padded too.

## Errors and panics

A write that fails comes back as an error, from `tabwriter_writer_write` or from `tabwriter_writer_flush`, and the count from a write says how much of the input was taken in before the failure. A failed flush empties the writer, so it can be used again. Running out of memory is an error in the same way, `burrow_err_out_of_memory`.

A panic inside `out` is passed on as Go passes it on, as a new panic whose text is `tabwriter: panic during Flush (...)` or `tabwriter: panic during Write (...)` with the original inside the brackets. Negative widths or padding panic in `tabwriter_new_writer` and `tabwriter_writer_init`, as they do in Go.
